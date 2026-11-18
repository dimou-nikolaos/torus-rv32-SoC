// I/O tile test (SPEC.md, I/O tile verification): AXI bursts, byte strobes,
// backpressure and read ordering against a golden memory model, across an
// asynchronous clock ratio (--ratio=0..5, else chosen by the seed).
//
// The host side is the AXI bus functional model, with random gaps and B/R
// backpressure. The NoC side models the rest of the grid on both planes: every
// injected flit is checked (source, lookahead port, the plane select_plane()
// gives, READ_REQ fields) and applied to a memory per tile; READ_REQs are
// answered with READ_RSP flits after a random delay, in order per tile but
// interleaved across tiles, each tile on its plane towards the I/O tile;
// injection credits come back late at random; tiles send ATOMIC_INC and WRITE
// flits to the I/O register block, plus flits the I/O tile must drop.
//
// The host keeps a shadow of everything it wrote. A read may not overlap a
// write still in flight (and the other way round); every read must then
// return the shadow, beat for beat. At the end the tile memories equal the
// shadow, and the I/O counters, semaphores and INFO match the model. Bursts
// to bad addresses, sizes or burst types answer SLVERR and send nothing.
// Throughput: a long write burst injects one flit per fabric cycle (or at the
// AXI beat rate if that is slower), and streamed reads keep up the same.
#include <verilated.h>
#include <verilated_fst_c.h>

#include <array>
#include <deque>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "Vtb_io_top.h"
#include "axi_bfm.h"
#include "noc_flit.h"
#include "noc_tlm.h"
#include "router_model.h"
#include "tb_args.h"

using namespace torus;

namespace {

constexpr int kGX = 4, kGY = 4, kIoX = 0, kIoY = 3, kDepth = 5, kOut = 8;
constexpr uint32_t kIoRegs = 0x10004000u;

uint32_t gaddr(int x, int y, uint32_t win, uint32_t off) {
  return (uint32_t)x << 28 | (uint32_t)y << 24 | win << 20 | (off & 0xffff);
}
uint32_t io_reg(uint32_t off) { return gaddr(kIoX, kIoY, 1, 0x4000 + off); }

// Byte-addressed memory over global addresses, with a deterministic initial
// value so the host shadow and the tile model agree on untouched bytes
struct Mem {
  std::unordered_map<uint32_t, std::array<uint8_t, 16>> m;
  std::array<uint8_t, 16>& line(uint32_t a) {
    auto it = m.find(a & ~15u);
    if (it != m.end()) return it->second;
    std::array<uint8_t, 16> l;
    for (int i = 0; i < 16; i++) l[i] = (uint8_t)(((a & ~15u) + i) * 2654435761u >> 24);
    return m.emplace(a & ~15u, l).first->second;
  }
  void write(uint32_t a, const Beat& b, uint16_t strb) {
    auto& l = line(a);
    const uint8_t* d = (const uint8_t*)b.w;
    for (int i = 0; i < 16; i++) if (strb >> i & 1) l[i] = d[i];
  }
  Beat read(uint32_t a) {
    Beat b;
    memcpy(b.w, line(a).data(), 16);
    return b;
  }
};

struct Range {
  uint32_t lo, hi;
};
bool overlap(const Range& a, const Range& b) { return a.lo < b.hi && b.lo < a.hi; }

int plane(int sx, int sy, int dx, int dy) { return select_plane(kGX, kGY, sx, sy, dx, dy); }

template <typename W>
void put_flit(W& vec, int p, const Flit& f) {
  uint32_t w[Flit::kWords];
  f.pack(w);
  for (int b = 0; b < Flit::kBits; b++) {
    int bit = p * Flit::kBits + b;
    if (w[b / 32] >> (b % 32) & 1) vec[bit / 32] |= 1u << (bit % 32);
    else vec[bit / 32] &= ~(1u << (bit % 32));
  }
}
template <typename W>
Flit get_flit(const W& vec, int p) {
  uint32_t w[Flit::kWords] = {};
  for (int b = 0; b < Flit::kBits; b++) {
    int bit = p * Flit::kBits + b;
    if (vec[bit / 32] >> (bit % 32) & 1) w[b / 32] |= 1u << (b % 32);
  }
  return Flit::unpack(w);
}

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 300000);
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randReset(2);
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_io_top>(ctx.get());
  std::unique_ptr<VerilatedFstC> tfp;
  if (!args.trace.empty()) {
    ctx->traceEverOn(true);
    tfp = std::make_unique<VerilatedFstC>();
    dut->trace(tfp.get(), 99);
    tfp->open(args.trace.c_str());
  }

  // Clocks: AXI and fabric half periods, per seed
  const std::pair<int, int> ratios[] = {{5, 5}, {3, 5}, {7, 5}, {4, 9}, {11, 5}, {5, 6}};
  size_t ri = args.flag("ratio") ? (size_t)std::stoul(args.opt("ratio")) : (size_t)(args.seed % 6);
  auto [ah, fh] = ratios[ri % (sizeof ratios / sizeof ratios[0])];
  printf("AXI half period %d, fabric half period %d\n", ah, fh);
  uint64_t now = 0, anext = 1, fnext = 2 + rng() % (uint64_t)fh;
  uint64_t fcycles = 0, acycles = 0;

  AxiMaster<Vtb_io_top> bfm(dut.get(), args.seed ^ 0x5eed);

  // ------------------------------------------------------------ NoC model
  Mem tiles;                     // what the tiles hold (global addresses)
  struct Reply {
    uint64_t ready;              // fabric cycle it may be sent
    Flit f;
    bool last;                   // the request's last reply
  };
  std::deque<Reply> replies[kGX * kGY];
  std::deque<int> inj_credit_delay[2];
  std::deque<Flit> tile_traffic[2];  // to the I/O tile, besides replies, per plane
  int ej_credits[2] = {kDepth, kDepth};
  uint64_t ej_flits[2] = {0, 0}, inj_per_plane[2] = {0, 0};
  int rd_in_flight = 0, max_rd_in_flight = 0;
  int reply_lat = 10;
  int credit_jitter = 6;
  bool slow_credits = true;
  uint64_t n_writes = 0, n_readreq = 0, n_rsp = 0, n_bad = 0, n_atomic = 0, inj_flits = 0, n_rx_all = 0;
  uint32_t sem_sum[16] = {};     // atomics to SEM 8..11, writes to SEM 12..15
  uint32_t sem_last[16] = {};
  for (int k = 12; k < 16; k++) sem_last[k] = 0;
  int traffic_pct = 0;           // no tile traffic until reset is over

  auto net_drive = [&]() {
    dut->inj_credit_i = 0;
    dut->ej_valid_i = 0;
    for (int p = 0; p < 2; p++) {
      auto& cd = inj_credit_delay[p];
      if (!cd.empty() && cd.front() <= 0) {
        dut->inj_credit_i |= (uint8_t)(1u << p);
        cd.pop_front();
      }
    }
    auto send = [&](int p, const Flit& f) {
      put_flit(dut->ej_flit_i, p, f);
      dut->ej_valid_i |= (uint8_t)(1u << p);
      ej_credits[p]--;
      ej_flits[p]++;
      n_rx_all++;
    };
    if (tile_traffic[0].empty() && tile_traffic[1].empty() && (int)(rng() % 100) < traffic_pct) {
      Flit f;
      f.src_x = (uint8_t)(rng() % kGX);
      f.src_y = (uint8_t)(rng() % (kGY - 1));
      f.dst_x = kIoX;
      f.dst_y = kIoY;
      f.tag = (uint8_t)rng();
      for (auto& d : f.data) d = (uint32_t)rng();
      int k = (int)(rng() % 10);
      if (k < 5) {  // semaphore increment
        int s = 8 + (int)(rng() % 4);
        f.op = Op::AtomicInc;
        f.addr = kIoRegs + 4u * (uint32_t)s;
        f.data[0] &= 0xffff;
        sem_sum[s] += f.data[0];
        n_atomic++;
      } else if (k < 8) {  // a tile writes a whole semaphore word
        int s = 12 + (int)(rng() % 4);
        f.op = Op::Write;
        f.addr = kIoRegs + (uint32_t)(s & ~3) * 4;
        f.be = (uint16_t)(0xF << (4 * (s & 3)));
        sem_last[s] = f.data[s & 3];
      } else {  // dropped: a read, a write outside the register page, a misaligned atomic
        switch (rng() % 3) {
          case 0: f.op = Op::ReadReq; f.addr = kIoRegs; f.data[1] = 16; break;
          case 1: f.op = Op::Write; f.addr = 0x1000; f.be = 0xffff; break;
          default: f.op = Op::AtomicInc; f.addr = kIoRegs + 2; break;
        }
        n_bad++;
      }
      tile_traffic[plane(f.src_x, f.src_y, kIoX, kIoY)].push_back(f);
    }
    for (int p = 0; p < 2; p++) {
      if (ej_credits[p] <= 0) continue;
      // A reply whose tile is ready (and answers on this plane), else tile traffic
      int start = (int)(rng() % (kGX * kGY));
      bool sent = false;
      for (int i = 0; i < kGX * kGY && !sent; i++) {
        int n = (start + i) % (kGX * kGY);
        auto& q = replies[n];
        if (q.empty() || q.front().ready > fcycles || plane(n % kGX, n / kGX, kIoX, kIoY) != p) continue;
        if (q.front().last) rd_in_flight--;   // the slot frees later still: an upper bound
        send(p, q.front().f);
        q.pop_front();
        n_rsp++;
        sent = true;
      }
      if (!sent && !tile_traffic[p].empty()) {
        send(p, tile_traffic[p].front());
        tile_traffic[p].pop_front();
      }
    }
  };

  // A flit the I/O tile injected on plane p
  auto inject = [&](int p, const Flit& f, int la) -> int {
    inj_flits++;
    inj_per_plane[p]++;
    TB_CHECK(f.src_x == kIoX && f.src_y == kIoY, "flit from (%u,%u)", f.src_x, f.src_y);
    TB_CHECK(f.dst_x < kGX && f.dst_y < kGY - 1, "flit to (%u,%u)", f.dst_x, f.dst_y);
    TB_CHECK(la == (int)route(kIoX, kIoY, f.dst_x, f.dst_y), "lookahead port %d", la);
    TB_CHECK(p == plane(kIoX, kIoY, f.dst_x, f.dst_y), "flit to (%u,%u) on plane %d", f.dst_x, f.dst_y, p);
    bool reg = f.addr >= 0x10000000u;
    TB_CHECK(reg ? ((f.addr >> 12) == 0x10001 || (f.addr >> 12) == 0x10002) : f.addr < 0x10000,
             "flit to local address %08x", f.addr);
    uint32_t g = gaddr(f.dst_x, f.dst_y, reg ? 1 : 0, f.addr);
    if (f.op == Op::Write) {
      TB_CHECK(f.be != 0 && !(f.addr & 15), "WRITE with be %04x at %08x", f.be, f.addr);
      Beat b;
      memcpy(b.w, f.data, 16);
      tiles.write(g, b, f.be);
      n_writes++;
    } else if (f.op == Op::ReadReq) {
      uint32_t ret = f.data[0], len = f.data[1];
      TB_CHECK(!(ret & 63) && ret / 64 < kOut && f.tag == ret / 64 && len >= 16 && len <= 64 && !(len & 15) &&
                   !(f.addr & 15) && !f.data[2] && !f.data[3],
               "READ_REQ ret %08x len %u tag %02x addr %08x", ret, len, f.tag, f.addr);
      TB_CHECK(++rd_in_flight <= kOut, "%d READ_REQs in flight", rd_in_flight);
      max_rd_in_flight = std::max(max_rd_in_flight, rd_in_flight);
      n_readreq++;
      auto& q = replies[f.dst_y * kGX + f.dst_x];
      uint64_t ready = fcycles + (uint64_t)reply_lat + rng() % (uint64_t)(reply_lat + 1);
      if (!q.empty()) ready = std::max(ready, q.back().ready);
      for (uint32_t i = 0; i < len / 16; i++) {
        Flit r;
        r.src_x = f.dst_x;
        r.src_y = f.dst_y;
        r.dst_x = kIoX;
        r.dst_y = kIoY;
        r.op = Op::ReadRsp;
        r.addr = ret + 16 * i;
        r.be = 0xffff;
        r.tag = f.tag;
        Beat b = tiles.read(g + 16 * i);
        memcpy(r.data, b.w, 16);
        q.push_back({ready, r, i + 1 == len / 16});
      }
    } else {
      TB_CHECK(false, "unexpected op %u", (unsigned)f.op);
    }
    int d = slow_credits ? 1 + (int)(rng() % (credit_jitter + 1)) : 1;
    inj_credit_delay[p].push_back(d);
    return 0;
  };

  auto net_sample = [&]() -> int {
    for (int p = 0; p < 2; p++)
      if (dut->ej_credit_o >> p & 1) ej_credits[p]++;
    for (int p = 0; p < 2; p++)
      if (dut->inj_valid_o >> p & 1)
        if (inject(p, get_flit(dut->inj_flit_o, p), (dut->inj_la_o >> (2 * p)) & 3)) return 1;
    return 0;
  };

  // ------------------------------------------------------------ clocking
  auto eval = [&]() {
    dut->eval();
    if (tfp) tfp->dump(now);
  };
  auto step = [&]() -> int {
    uint64_t t = std::min(anext, fnext);
    now = t;
    bool a = anext == t, f = fnext == t;
    bool arise = a && !dut->aclk_i, frise = f && !dut->clk_i;
    if (arise) bfm.drive();
    if (frise) net_drive();
    eval();
    if (arise) TB_CHECK(bfm.sample(), "AXI: %s", bfm.error().c_str());
    if (frise && net_sample()) return 1;
    if (a) {
      dut->aclk_i = !dut->aclk_i;
      anext += (uint64_t)ah;
      if (arise) acycles++;
    }
    if (f) {
      dut->clk_i = !dut->clk_i;
      fnext += (uint64_t)fh;
      if (frise) {
        fcycles++;
        for (auto& cd : inj_credit_delay)
          for (auto& d : cd) d--;
      }
    }
    eval();
    return ctx->gotError() ? 1 : 0;
  };

  // Reset both domains
  ctx->assertOn(false);
  dut->aclk_i = 0;
  dut->clk_i = 0;
  dut->arst_ni = 1;
  dut->rst_ni = 1;
  dut->inj_credit_i = 0;
  dut->ej_valid_i = 0;
  eval();
  dut->arst_ni = 0;
  dut->rst_ni = 0;
  eval();
  for (int i = 0; i < 40; i++) if (step()) return 1;
  dut->arst_ni = 1;
  dut->rst_ni = 1;
  eval();
  ctx->assertOn(true);
  traffic_pct = 3;

  // ------------------------------------------------------------ host side
  Mem shadow;
  uint32_t sem_host[8] = {};
  struct Op_ {
    int h;
    Range r;
    bool rd, bad;
    std::vector<Beat> expect;
  };
  std::vector<Op_> inflight;
  uint64_t n_ops = 0, n_bad_ops = 0, beats_checked = 0;

  auto busy_with = [&](const Range& r, bool want_read) {
    for (auto& o : inflight) if (o.rd != want_read && !o.bad && overlap(o.r, r)) return true;
    return false;
  };
  auto random_beat = [&]() {
    Beat b;
    for (auto& x : b.w) x = (uint32_t)rng();
    return b;
  };

  // A random good target: a compute tile's scratchpad (most), its tile control
  // or counter bank page, or the I/O register block. Bursts stay in 4 KB.
  auto pick = [&](unsigned max_beats, uint32_t* addr, unsigned* beats) {
    int k = (int)(rng() % 20);
    if (k < 15) {
      int x = (int)(rng() % kGX), y = (int)(rng() % (kGY - 1));
      // A few hot lines so reads and writes meet, and spread elsewhere
      uint32_t off = rng() % 4 ? (uint32_t)(rng() % 2048) * 16 : (uint32_t)(rng() % 8) * 16;
      *addr = gaddr(x, y, 0, off) + (uint32_t)(rng() % 16 == 0 ? rng() % 16 : 0);
      unsigned room = (4096 - (off & 4095)) / 16;
      *beats = 1 + (unsigned)(rng() % std::min(max_beats, room));
      return 0;
    }
    if (k < 18) {
      int x = (int)(rng() % kGX), y = (int)(rng() % (kGY - 1));
      uint32_t off = (rng() & 1 ? 0x1000u : 0x2000u) + (uint32_t)(rng() % 256) * 16;
      *addr = gaddr(x, y, 1, off);
      unsigned room = (4096 - (off & 4095)) / 16;
      *beats = 1 + (unsigned)(rng() % std::min(4u, room));
      return 0;
    }
    *addr = io_reg(0);   // host semaphores 0..7: two beats
    *beats = 1 + (unsigned)(rng() % 2);
    return 1;
  };

  auto issue_random = [&]() {
    uint32_t addr;
    unsigned beats;
    bool rd = rng() % 2;
    int bad_kind = rng() % 16 == 0 ? 1 + (int)(rng() % 7) : 0;
    bool io = pick(16, &addr, &beats);
    if (bad_kind) {
      switch (bad_kind) {
        case 1: addr = gaddr(1, kIoY, 0, 0); break;           // router-only stop
        case 2: addr = gaddr(kGX, 0, 0, 0); break;            // outside the grid
        case 3: addr = gaddr(0, 0, 2, 0); break;              // no such window
        case 4: addr = gaddr(1, 1, 1, 0x0000); break;         // NIU registers: core only
        case 5: addr = gaddr(kIoX, kIoY, 0, 0x100); break;    // the I/O tile has no scratchpad
        case 6: bfm.size = 3; break;                          // narrow beats
        default: bfm.burst = 0; break;                        // FIXED burst
      }
      io = false;
    }
    Range r{addr & ~15u, (addr & ~15u) + 16 * beats};
    if (!bad_kind && busy_with(r, rd)) return;
    Op_ o{0, r, rd, bad_kind != 0, {}};
    if (rd) {
      o.h = bfm.read(addr, beats, (unsigned)(rng() % 16));
      if (!bad_kind)
        for (unsigned i = 0; i < beats; i++) {
          if (io) {
            Beat b;
            for (int l = 0; l < 4; l++) b.w[l] = 4 * i + l < 8 ? sem_host[4 * i + l] : 0;
            o.expect.push_back(b);
          } else {
            o.expect.push_back(shadow.read(r.lo + 16 * i));
          }
        }
    } else {
      std::vector<Beat> data;
      std::vector<uint16_t> strb;
      for (unsigned i = 0; i < beats; i++) {
        data.push_back(random_beat());
        uint16_t s = rng() % 3 ? 0xffff : (uint16_t)rng();
        if (i == 0 && (addr & 15)) s &= (uint16_t)(0xffff << (addr & 15));  // bytes below an unaligned start
        strb.push_back(s);
        if (!bad_kind) {
          if (io) {
            for (int l = 0; l < 4; l++) {
              uint32_t& v = sem_host[4 * i + l];
              for (int b = 0; b < 4; b++)
                if (s >> (4 * l + b) & 1) v = (v & ~(0xffu << (8 * b))) | (data.back().w[l] & (0xffu << (8 * b)));
            }
          } else {
            shadow.write(r.lo + 16 * i, data.back(), s);
          }
        }
      }
      o.h = bfm.write(addr, data, strb, (unsigned)(rng() % 16));
    }
    bfm.size = 4;
    bfm.burst = 1;
    inflight.push_back(std::move(o));
    n_ops++;
    if (bad_kind) n_bad_ops++;
  };

  auto retire = [&]() -> int {
    for (size_t i = 0; i < inflight.size();) {
      Op_& o = inflight[i];
      if (!bfm.done(o.h)) { i++; continue; }
      const AxiResult& res = bfm.result(o.h);
      TB_CHECK(res.resp == (o.bad ? 2u : 0u), "%s at %08x: resp %u, expected %s", o.rd ? "read" : "write", o.r.lo,
               res.resp, o.bad ? "SLVERR" : "OKAY");
      if (o.rd) {
        TB_CHECK(res.data.size() == (o.r.hi - o.r.lo) / 16, "read at %08x: %zu beats", o.r.lo, res.data.size());
        for (size_t b = 0; b < res.data.size(); b++) {
          const Beat& e = o.bad ? Beat{} : o.expect[b];
          TB_CHECK(memcmp(&res.data[b], &e, 16) == 0,
                   "read at %08x beat %zu: %08x %08x %08x %08x, expected %08x %08x %08x %08x", o.r.lo, b,
                   res.data[b].w[0], res.data[b].w[1], res.data[b].w[2], res.data[b].w[3], e.w[0], e.w[1], e.w[2],
                   e.w[3]);
          beats_checked++;
        }
      }
      bfm.forget(o.h);
      inflight.erase(inflight.begin() + (long)i);
    }
    return 0;
  };
  auto settle = [&](uint64_t max_steps) -> int {
    for (uint64_t i = 0; i < max_steps && !(bfm.idle_all() && inflight.empty()); i++) {
      if (step()) return 1;
      if (retire()) return 1;
    }
    TB_CHECK(bfm.idle_all() && inflight.empty(), "%zu operations did not complete", inflight.size());
    return 0;
  };

  // ------------------------------------------------- 1. random traffic
  bfm.gap_pct = 30;
  bfm.bp_pct = 30;
  for (uint64_t s = 0; s < args.cycles; s++) {
    if (inflight.size() < 12 && rng() % 4 == 0) issue_random();
    if (step()) return 1;
    if (retire()) return 1;
    if (s % 50000 == 0) {  // vary the network: reply latency and credit return
      reply_lat = 2 + (int)(rng() % 40);
      slow_credits = rng() % 3 != 0;
    }
  }
  traffic_pct = 0;
  if (settle(200000)) return 1;
  for (int i = 0; i < 4000; i++) if (step()) return 1;   // late replies and tile traffic drain
  printf("random: %llu operations (%llu bad), %llu read beats checked, %llu flits, up to %d READ_REQs in flight\n",
         (unsigned long long)n_ops, (unsigned long long)n_bad_ops, (unsigned long long)beats_checked,
         (unsigned long long)inj_flits, max_rd_in_flight);

  // ------------------------------------------------- 2. throughput
  bfm.gap_pct = 0;
  bfm.bp_pct = 0;
  slow_credits = false;
  reply_lat = 8;
  // The beat rate the slower clock allows, per fabric cycle
  double limit = std::min(1.0, (double)fh / ah);
  // Write stream rate, measured directly: one burst, flits per fabric cycle
  auto write_rate = [&]() -> double {
    std::vector<Beat> data(256);
    std::vector<uint16_t> strb(256, 0xffff);
    for (auto& b : data) b = random_beat();
    for (unsigned i = 0; i < 256; i++) shadow.write(gaddr(3, 0, 0, 0x4000) + 16 * i, data[i], 0xffff);
    uint64_t n_start = inj_flits;
    int h = bfm.write(gaddr(3, 0, 0, 0x4000), data, strb);
    uint64_t fa = 0, na = 0;
    while (inj_flits - n_start < 224) {
      if (step()) return -1;
      if (!fa && inj_flits - n_start >= 32) { fa = fcycles; na = inj_flits; }
    }
    double r = (double)(inj_flits - na) / (double)(fcycles - fa);
    while (!bfm.done(h)) if (step()) return -1;
    bfm.forget(h);
    return r;
  };
  double wr = write_rate();
  TB_CHECK(wr > 0.95 * limit, "write stream: %.3f flits per fabric cycle, limit %.3f", wr, limit);
  // Read stream: 32 bursts of 16 beats, kept outstanding
  auto read_rate = [&]() -> double {
    std::vector<int> hs;
    for (int i = 0; i < 32; i++) hs.push_back(bfm.read(gaddr(i % kGX, (i / kGX) % (kGY - 1), 0, 0x100u * (uint32_t)i), 16));
    uint64_t b0 = bfm.r_beats, fa = 0, ba = 0;
    while (bfm.r_beats - b0 < 448) {
      if (step()) return -1;
      if (!fa && bfm.r_beats - b0 >= 64) { fa = fcycles; ba = bfm.r_beats; }
    }
    double r = (double)(bfm.r_beats - ba) / (double)(fcycles - fa);
    for (int h : hs) {
      while (!bfm.done(h)) if (step()) return -1;
      bfm.forget(h);
    }
    return r;
  };
  double rr = read_rate();
  printf("throughput: write %.3f, read %.3f beats per fabric cycle (limit %.3f, reply latency %d)\n", wr, rr, limit,
         reply_lat);
  TB_CHECK(rr > 0.9 * limit, "read stream: %.3f beats per fabric cycle, limit %.3f", rr, limit);

  // ------------------------------------------------- 3. final state
  for (int i = 0; i < 2000; i++) if (step()) return 1;
  for (auto& [a, l] : shadow.m) {
    auto& t = tiles.line(a);
    TB_CHECK(memcmp(l.data(), t.data(), 16) == 0, "memory at %08x differs from the shadow", a);
  }
  auto rd32 = [&](uint32_t off, uint32_t* v) -> int {
    int h = bfm.read(io_reg(off & ~15u), 1);
    for (int i = 0; i < 20000 && !bfm.done(h); i++) if (step()) return 1;
    TB_CHECK(bfm.done(h) && bfm.result(h).resp == 0, "I/O register read at %03x failed", off);
    *v = bfm.result(h).data[0].w[(off >> 2) & 3];
    bfm.forget(h);
    return 0;
  };
  uint32_t v;
  if (rd32(0x100, &v)) return 1;
  TB_CHECK(v == ((uint32_t)kOut << 24 | (uint32_t)kGY << 16 | (uint32_t)kGX << 8 | 1), "INFO %08x", v);
  for (int s = 0; s < 16; s++) {
    if (rd32(4u * (uint32_t)s, &v)) return 1;
    uint32_t e = s < 8 ? sem_host[s] : s < 12 ? sem_sum[s] : sem_last[s];
    TB_CHECK(v == e, "SEM[%d] = %08x, expected %08x", s, v, e);
  }
  struct { uint32_t off; uint64_t want; const char* name; } cnt[] = {
      {0x108, n_writes, "WR_FLITS"}, {0x10C, n_readreq, "RD_REQS"}, {0x110, n_rsp, "RSP_FLITS"},
      {0x114, n_bad, "RX_ERRORS"}, {0x118, n_atomic, "ATOMICS"}, {0x11C, n_rx_all, "RX_FLITS"}};
  for (auto& c : cnt) {
    if (rd32(c.off, &v)) return 1;
    TB_CHECK(v == (uint32_t)c.want, "%s = %u, expected %llu", c.name, v, (unsigned long long)c.want);
  }
  if (tfp) tfp->close();
  dut->final();
  TB_CHECK(inj_per_plane[0] && inj_per_plane[1] && ej_flits[0] && ej_flits[1], "a plane carried no traffic");
  printf("tb_io: PASS %llu operations, %llu flits (NoC0 %llu, NoC1 %llu; %llu writes, %llu READ_REQs, %llu replies), "
         "%llu tile flits (%llu atomics, %llu dropped), %llu fabric cycles\n",
         (unsigned long long)n_ops, (unsigned long long)inj_flits, (unsigned long long)inj_per_plane[0],
         (unsigned long long)inj_per_plane[1], (unsigned long long)n_writes,
         (unsigned long long)n_readreq, (unsigned long long)n_rsp, (unsigned long long)(n_atomic + n_bad),
         (unsigned long long)n_atomic, (unsigned long long)n_bad, (unsigned long long)fcycles);
  return 0;
}
