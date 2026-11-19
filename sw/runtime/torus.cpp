// libtorus v0 (see torus.h).
#include "torus.h"

#include <cstring>
#include <stdexcept>

namespace torus {

uint32_t global_addr(Xy t, uint32_t local) {
  uint32_t win;
  if (local < 0x10000) win = 0;
  else if ((local & 0xFFFF0000u) == 0x10000000u) win = 1;
  else throw std::runtime_error("no global address for local address " + std::to_string(local));
  return t.x << 28 | t.y << 24 | win << 20 | (local & 0xFFFF);
}

Device::Device(Transport& t) : t_(t) {
  // The I/O tile is at (0, NY); INFO tells GX and GY. Read it from (0, y) for
  // every y until the register block answers: the first INFO read must find
  // the I/O row without knowing NY.
  for (unsigned y = 1; y < 16; y++) {
    uint8_t beat[16];
    try {
      t_.read(global_addr({0, y}, kIoRegs + kIoInfo), beat, 1);
    } catch (const std::runtime_error&) {
      continue;   // a compute tile: no register page 4
    }
    uint32_t info;
    memcpy(&info, beat, 4);
    unsigned gx = info >> 8 & 0xff, gy = info >> 16 & 0xff;
    if ((info & 0xff) != 1 || gy != y + 1) throw std::runtime_error("unexpected I/O INFO " + std::to_string(info));
    nx_ = gx;
    ny_ = gy - 1;
    out_ = info >> 24;
    spm_ = read32({0, 0}, kTctrl + kTctrlSpm);
    if (spm_ != 32 * 1024 && spm_ != 64 * 1024)
      throw std::runtime_error("unexpected scratchpad size " + std::to_string(spm_));
    return;
  }
  throw std::runtime_error("no I/O tile found");
}

std::vector<Xy> Device::compute_grid() const {
  std::vector<Xy> g;
  for (unsigned y = 0; y < ny_; y++)
    for (unsigned x = 0; x < nx_; x++) g.push_back({x, y});
  return g;
}

void Device::write(Xy t, uint32_t local, const void* data, size_t len) {
  const uint8_t* src = (const uint8_t*)data;
  uint32_t a = global_addr(t, local);
  while (len) {
    // One burst: up to the next 4 KB boundary, at most 256 beats
    uint32_t base = a & ~15u, head = a & 15u;
    uint32_t to_4k = 4096 - (base & 4095);
    size_t n = std::min<size_t>(len, std::min<size_t>(to_4k, 256 * 16) - head);
    unsigned beats = (unsigned)((head + n + 15) / 16);
    std::vector<uint8_t> buf(16 * beats, 0);
    std::vector<uint16_t> strb(beats, 0);
    memcpy(&buf[head], src, n);
    for (size_t i = head; i < head + n; i++) strb[i / 16] |= (uint16_t)(1u << (i % 16));
    t_.write(base, buf.data(), strb.data(), beats);
    a += (uint32_t)n;
    src += n;
    len -= n;
  }
}

void Device::read(Xy t, uint32_t local, void* data, size_t len) {
  t_.sync();   // a read must see every earlier write
  uint8_t* dst = (uint8_t*)data;
  uint32_t a = global_addr(t, local);
  while (len) {
    uint32_t base = a & ~15u, head = a & 15u;
    uint32_t to_4k = 4096 - (base & 4095);
    size_t n = std::min<size_t>(len, std::min<size_t>(to_4k, 256 * 16) - head);
    unsigned beats = (unsigned)((head + n + 15) / 16);
    std::vector<uint8_t> buf(16 * beats);
    t_.read(base, buf.data(), beats);
    memcpy(dst, &buf[head], n);
    a += (uint32_t)n;
    dst += n;
    len -= n;
  }
}

uint32_t Device::read32(Xy t, uint32_t local) {
  uint32_t v;
  read(t, local, &v, 4);
  return v;
}

Program Device::load_kernel(const std::string& elf, const std::vector<Xy>& tiles) {
  return load_kernels(std::vector<std::string>(tiles.size(), elf), tiles);
}

Program Device::load_kernels(const std::vector<std::string>& elfs, const std::vector<Xy>& tiles) {
  if (elfs.size() != tiles.size()) throw std::runtime_error("load_kernels: one ELF per tile");
  Program p;
  p.tiles = tiles;
  std::map<std::string, ElfImage> cache;
  for (auto& e : elfs) {
    if (!cache.count(e)) cache[e] = load_elf(e);
    p.images.push_back(cache[e]);
  }
  for (size_t i = 0; i < tiles.size(); i++) {
    Xy t = tiles[i];
    write32(t, kTctrl + kTctrlCtrl, 0);   // hold the core in reset
    for (auto& s : p.images[i].segments) {
      if (s.addr + s.bytes.size() > kMailbox) throw std::runtime_error("segment does not fit below the mailbox");
      write(t, s.addr, s.bytes.data(), s.bytes.size());
    }
    write32(t, kExitWord, 0);
  }
  return p;
}

void Device::install_loader(const std::string& loader_elf, const std::vector<Xy>& tiles) {
  loader_ = load_kernels(std::vector<std::string>(tiles.size(), loader_elf), tiles);
  std::vector<uint8_t> zero(256 - kLoaderTableSent, 0);   // its counters, kept across loads
  for (Xy t : tiles) write(t, kLoaderTable + kLoaderTableSent, zero.data(), zero.size());
}

Program Device::load_kernel_tree(const std::string& elf, const std::vector<Xy>& tiles) {
  if (loader_.tiles != tiles || tiles != compute_grid())
    throw std::runtime_error("load_kernel_tree: install the loader on the whole compute grid first");
  Program p;
  p.tiles = tiles;
  p.images.assign(tiles.size(), load_elf(elf));
  const ElfImage& img = p.images[0];
  // The table: segments widened to 16-byte words, and the image's bytes 0..15
  // (address 0 holds the jump stub while the loader runs)
  struct {
    uint32_t nseg, head[4], seg[12][2], pad[3];
  } tab{};
  static_assert(sizeof tab == kLoaderTableSent, "loader table layout (sw/kernels/loader)");
  bool has_head = false;
  for (auto& s : img.segments) {
    uint32_t lo = s.addr & ~15u, hi = (uint32_t)(s.addr + s.bytes.size() + 15) & ~15u;
    if (hi > kLoaderBase) throw std::runtime_error("load_kernel_tree: segment reaches the loader at 0x6800");
    if (tab.nseg == 12) throw std::runtime_error("load_kernel_tree: more than 12 segments");
    tab.seg[tab.nseg][0] = lo;
    tab.seg[tab.nseg][1] = hi - lo;
    tab.nseg++;
    if (s.addr == 0 && s.bytes.size() >= 16) {
      memcpy(tab.head, s.bytes.data(), 16);
      has_head = true;
    }
  }
  if (!has_head) throw std::runtime_error("load_kernel_tree: the image does not start at address 0");
  Xy root = tiles[0];
  for (auto& s : img.segments) write(root, s.addr, s.bytes.data(), s.bytes.size());
  write(root, kLoaderTable, &tab, sizeof tab);
  // Every tile boots into the loader at 0x6800: lui t0, 0x7; jalr x0, -2048(t0); nop; nop
  static_assert(kLoaderBase == 0x7000 - 2048, "jump stub");
  const uint32_t stub[4] = {0x000072B7u, 0x80028067u, 0x00000013u, 0x00000013u};
  for (Xy t : tiles) write(t, 0, stub, sizeof stub);
  // Run the loader: no counter clear and no settling wait after the stop
  // writes, the kernel's own launch clears the counters; every later write to
  // a tile follows its stop write on the same path
  io_write32(kIoDone, 0);
  for (size_t i = tiles.size(); i-- > 0;) write32(tiles[i], kTctrl + kTctrlCtrl, 1);
  while (done_count() < tiles.size()) {
  }
  for (size_t i = tiles.size(); i-- > 0;) write32(tiles[i], kTctrl + kTctrlCtrl, 0);
  t_.sync();
  return p;
}

void Device::set_args(const Program&, Xy t, const void* args, size_t len) {
  if (len > kMailboxBytes) throw std::runtime_error("set_args: mailbox holds 240 bytes");
  write(t, kMailbox, args, len);
}

void Device::launch(const Program& p) {
  io_write32(kIoDone, 0);
  for (Xy t : p.tiles) write32(t, kPerf + kPerfCmd, kPerfClear);
  // Images, args, the counter clear and the run bit all travel from the I/O
  // tile to the same tile, in order: the core starts on a complete image.
  // Last tile first: a run write only passes tiles that are not running yet,
  // so no tile's counters see the launch traffic.
  for (size_t i = p.tiles.size(); i-- > 0;) write32(p.tiles[i], kTctrl + kTctrlCtrl, 1);
  t_.sync();
}

bool Device::wait(const Program& p, uint64_t max_polls) {
  for (uint64_t i = 0; i < max_polls; i++)
    if (done_count() >= p.tiles.size()) return true;
  return false;
}

std::vector<int> Device::exit_codes(const Program& p) {
  std::vector<int> codes;
  for (Xy t : p.tiles) {
    uint32_t w = read32(t, kExitWord);
    codes.push_back((w >> 28) == 0xE ? (int)(int16_t)(w & 0xFFFF) : -1);
  }
  return codes;
}

void Device::stop(const Program& p) {
  // Last tile first, as in launch: every stop write passes only tiles that
  // still run, so each tile's counters see exactly the stop writes routed
  // through it (and its own)
  for (size_t i = p.tiles.size(); i-- > 0;) write32(p.tiles[i], kTctrl + kTctrlCtrl, 0);
  t_.sync();
  // Return only once every stop write has been retired, so nothing the host
  // sends next reaches a tile that still counts: wait on the I/O tile's own
  // cycle counter (no NoC traffic) for longer than any flit takes
  wait_cycles(256 + 8 * (nx_ + ny_));
}

void Device::wait_cycles(uint32_t n) {
  uint32_t c0 = io_read32(kIoCycles);
  while (io_read32(kIoCycles) - c0 < n) {
  }
}

std::string Device::verify(const Program& p) {
  for (size_t i = 0; i < p.tiles.size(); i++)
    for (auto& s : p.images[i].segments) {
      std::vector<uint8_t> got(s.bytes.size());
      read(p.tiles[i], s.addr, got.data(), got.size());
      for (size_t k = 0; k < got.size(); k++)
        if (got[k] != s.bytes[k]) {
          char m[128];
          snprintf(m, sizeof m, "tile (%u,%u) at 0x%zx: 0x%02x, image 0x%02x", p.tiles[i].x, p.tiles[i].y,
                   s.addr + k, got[k], s.bytes[k]);
          return m;
        }
    }
  return "";
}

void Device::select_counters(const std::vector<Xy>& tiles, const std::vector<unsigned>& events) {
  for (Xy t : tiles)
    for (size_t k = 0; k < events.size(); k++) write32(t, kPerf + kPerfSel + 4 * (uint32_t)k, events[k]);
}

static std::vector<uint64_t> unpack_counters(const std::vector<uint32_t>& w) {
  std::vector<uint64_t> c;
  for (size_t i = 0; i + 1 < w.size(); i += 2) c.push_back((uint64_t)w[i + 1] << 32 | w[i]);
  return c;
}

std::vector<std::vector<uint64_t>> Device::read_counters(const std::vector<Xy>& tiles) {
  std::vector<std::vector<uint64_t>> r;
  for (Xy t : tiles) {
    uint32_t n = read32(t, kPerf + kPerfNum);
    write32(t, kPerf + kPerfCmd, kPerfSnapshot);
    std::vector<uint32_t> w(2 * n);
    read(t, kPerf + kPerfSnap, w.data(), 8 * n);   // after the snapshot: same path, in order
    r.push_back(unpack_counters(w));
  }
  return r;
}

std::vector<std::vector<uint64_t>> Device::read_live_counters(const std::vector<Xy>& tiles) {
  std::vector<std::vector<uint64_t>> r;
  for (Xy t : tiles) {
    uint32_t n = read32(t, kPerf + kPerfNum);
    std::vector<uint32_t> w(2 * n);
    read(t, kPerf, w.data(), 8 * n);   // low word first: each counter is one consistent value
    r.push_back(unpack_counters(w));
  }
  return r;
}

}  // namespace torus
