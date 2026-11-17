// libtorus v0: the C++ host runtime (SPEC.md, Software stack). It drives the
// I/O tile's AXI slave through a Transport (the Verilator model in simulation,
// QDMA on FPGA) and implements the launch protocol:
//
//   torus::Device dev(transport);
//   auto prog = dev.load_kernel("memcpy.elf", dev.compute_grid());
//   for (auto t : prog.tiles) dev.set_args(prog, t, &args, sizeof args);
//   dev.launch(prog);
//   dev.wait(prog);                       // DONE counter in the I/O tile
//   dev.read(t, C_ADDR, out, len);
//   auto perf = dev.read_counters(prog.tiles);
//
// Addresses are local addresses at a tile (scratchpad 0x0000_0000..0xFFFF,
// registers 0x1000_xxxx), turned into global AXI addresses here.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "elf_loader.h"

namespace torus {

struct Xy {
  unsigned x = 0, y = 0;
  bool operator==(const Xy& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Xy& o) const { return !(*this == o); }
};

// AXI memory-mapped access to the I/O tile. Bursts of 16-byte beats: addr is
// 16-byte aligned, at most 256 beats, never across a 4 KB boundary. write may
// return before its response (posted); sync() waits for every write. read
// returns with the data. A SLVERR response throws std::runtime_error.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void write(uint32_t addr, const uint8_t* data, const uint16_t* strb, unsigned beats) = 0;
  virtual void read(uint32_t addr, uint8_t* data, unsigned beats) = 0;
  virtual void sync() = 0;
};

// Global AXI address of a local address at tile t (io_pkg.sv)
uint32_t global_addr(Xy t, uint32_t local);

// Memory map constants shared with the tile SDK (sw/sdk/tile.h)
constexpr uint32_t kMailbox = 0xFF00, kMailboxBytes = 0xF0, kExitWord = 0xFFFC;
constexpr uint32_t kTctrl = 0x10001000, kPerf = 0x10002000, kIoRegs = 0x10004000;
constexpr uint32_t kTctrlCoord = 0x00, kTctrlCtrl = 0x04, kTctrlGrid = 0x14;
constexpr uint32_t kPerfSel = 0x400, kPerfSnap = 0x800, kPerfNum = 0xFF0, kPerfCmd = 0xFF4;
constexpr uint32_t kPerfSnapshot = 1, kPerfClear = 2;
constexpr uint32_t kIoDone = 0x000, kIoInfo = 0x100, kIoCycles = 0x104;

struct Program {
  std::vector<Xy> tiles;
  std::vector<ElfImage> images;   // one per tile, in the order of tiles
};

class Device {
 public:
  explicit Device(Transport& t);

  unsigned nx() const { return nx_; }
  unsigned ny() const { return ny_; }
  Xy io() const { return {0, ny_}; }
  unsigned io_rd_outstanding() const { return out_; }
  // Compute tiles in index order n = y * NX + x (the I/O row excluded)
  std::vector<Xy> compute_grid() const;

  // Any byte range at a tile, any alignment
  void write(Xy t, uint32_t local, const void* data, size_t len);
  void read(Xy t, uint32_t local, void* data, size_t len);
  void write32(Xy t, uint32_t local, uint32_t v) { write(t, local, &v, 4); }
  uint32_t read32(Xy t, uint32_t local);
  // The I/O tile's register block (offsets from 0x1000_4000)
  void io_write32(uint32_t off, uint32_t v) { write32(io(), kIoRegs + off, v); }
  uint32_t io_read32(uint32_t off) { return read32(io(), kIoRegs + off); }
  void sync() { t_.sync(); }

  // Launch protocol (SPEC.md): load holds the cores in reset (CTRL.run = 0)
  // and writes the images; set_args fills a tile's mailbox; launch clears DONE
  // and the counters and sets CTRL.run on every tile; wait polls DONE until
  // every tile has exited.
  Program load_kernel(const std::string& elf, const std::vector<Xy>& tiles);
  Program load_kernels(const std::vector<std::string>& elfs, const std::vector<Xy>& tiles);
  void set_args(const Program& p, Xy t, const void* args, size_t len);
  void launch(const Program& p);
  bool wait(const Program& p, uint64_t max_polls = ~0ull);
  uint32_t done_count() { return io_read32(kIoDone); }
  // Exit code of each tile (-1: no exit word)
  std::vector<int> exit_codes(const Program& p);
  // Holds every core in reset; returns once every tile has stopped
  void stop(const Program& p);
  // Waits n fabric cycles (the I/O tile's cycle counter)
  void wait_cycles(uint32_t n);
  // Reads back every loaded segment; returns the first mismatch, or "" if none
  std::string verify(const Program& p);

  // Counter banks: select the 8 programmable counters' events, and snapshot +
  // read all counters (counter i of tile k in result[k][i])
  void select_counters(const std::vector<Xy>& tiles, const std::vector<unsigned>& events);
  std::vector<std::vector<uint64_t>> read_counters(const std::vector<Xy>& tiles);
  std::vector<std::vector<uint64_t>> read_live_counters(const std::vector<Xy>& tiles);

 private:
  Transport& t_;
  unsigned nx_ = 0, ny_ = 0, out_ = 0;
};

}  // namespace torus
