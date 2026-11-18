// Scratchpad + bank arbiter unit test.
//
// Seven random masters drive spm_banks under OBI rules (a request is held until
// granted). Every cycle the C++ arbiter model must produce the same grants and
// promotions as the RTL, every read must return what the memory model holds,
// no request may wait beyond the starvation bound, and every request pattern
// (all 128 subsets of ports) must be seen on every bank.
#include <verilated.h>

#include <memory>
#include <random>
#include <set>

#include "Vtb_spm_top.h"
#include "spm_model.h"
#include "tb_args.h"
#include "tb_trace.h"

using namespace torus;

namespace {

constexpr int kStarveLimit = 4;  // matches the tb_spm_top default

void set_bits(uint32_t* w, int lsb, int width, uint64_t v) {
  for (int i = 0; i < width; i++) {
    int b = lsb + i;
    uint32_t m = 1u << (b % 32);
    if (v >> i & 1) w[b / 32] |= m;
    else w[b / 32] &= ~m;
  }
}

struct Req {
  bool valid = false, we = false, lock = false, add = false;
  uint32_t addr = 0;
  uint16_t be = 0;
  uint8_t data[16] = {};
  bool rmw_pending = false;  // next request must be the write half of an atomic
  int waited = 0;
};

enum class Mode { Uniform, HotBank, Stream };

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 200000);
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->commandArgs(argc, argv);
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_spm_top>(ctx.get());
  TbTrace tfp;
  tfp.open(ctx.get(), dut.get(), args.trace);

  std::vector<ArbiterModel> arb(kSpmBanks, ArbiterModel(kStarveLimit));
  SpmMemory mem;
  Req port[kSpmPorts];
  uint32_t stream_addr[kSpmPorts];
  for (int p = 0; p < kSpmPorts; p++) stream_addr[p] = (uint32_t)(rng() & 0xfff0);

  // Expected read data for the cycle after a grant
  bool exp_valid[kSpmPorts] = {};
  uint8_t exp_data[kSpmPorts][16];
  bool exp_is_read[kSpmPorts] = {};

  std::set<uint32_t> seen_masks[kSpmBanks];
  uint64_t grants = 0, promotions = 0, conflicts = 0, reads_checked = 0, adds = 0;
  int max_wait = 0;
  uint64_t t = 0;

  // Half a clock period in time steps; timed events in between (the SRAM macro
  // model's output delays in tb_spm_sky130) run at their own time.
  const uint64_t kHalf = 10;
  auto tick = [&](int clk) {
    dut->clk_i = clk;
    dut->eval();
    tfp.dump(ctx->time());
    const uint64_t end = ctx->time() + kHalf;
    while (dut->eventsPending() && dut->nextTimeSlot() < end) {
      ctx->time(dut->nextTimeSlot());
      dut->eval();
      tfp.dump(ctx->time());
    }
    ctx->time(end);
  };

  // Reset
  ctx->assertOn(false);  // random initial state until reset is applied
  dut->rst_ni = 1;
  dut->req_i = 0;
  dut->eval();
  dut->rst_ni = 0;
  tick(0); tick(1); tick(0); tick(1);
  dut->rst_ni = 1;
  ctx->assertOn(true);

  Mode mode = Mode::Uniform;
  int hot_bank = 0;
  auto new_request = [&](int p) {
    Req& r = port[p];
    r.add = false;
    if (r.rmw_pending) {  // write half of the atomic, same word: plain write or add
      r.valid = true;
      r.we = true;
      r.lock = false;
      r.add = rng() & 1;
      r.be = (uint16_t)(0xF << (4 * ((r.addr >> 2) & 3)));
      for (auto& b : r.data) b = (uint8_t)rng();
      r.rmw_pending = false;
      return;
    }
    double load = mode == Mode::HotBank ? 0.5 : 0.6;
    r.valid = std::uniform_real_distribution<>(0, 1)(rng) < load;
    if (!r.valid) return;
    r.we = rng() & 1;
    // RX ports occasionally issue an atomic: locked read, then write
    r.lock = (p == 1 || p == 2) && !r.we && (rng() % 4 == 0);
    switch (mode) {
      case Mode::Uniform: r.addr = (uint32_t)(rng() & 0xffff); break;
      case Mode::HotBank:
        r.addr = (uint32_t)((rng() & 0xffc0) | (hot_bank << 4) | (rng() & 0xf));
        break;
      case Mode::Stream:
        stream_addr[p] = (stream_addr[p] + 16) & 0xffff;
        r.addr = stream_addr[p];
        break;
    }
    if (p >= 5) {  // core ports: 32-bit lanes
      r.addr &= ~3u;
      int lane = (r.addr >> 2) & 3;
      r.be = (uint16_t)((rng() & 0xF) << (4 * lane));
      if (p == 6) { r.we = false; r.lock = false; r.be = (uint16_t)(0xF << (4 * lane)); }
    } else {
      r.be = (uint16_t)rng();
    }
    if (!r.we) r.be = r.lock ? (uint16_t)(0xF << (4 * ((r.addr >> 2) & 3))) : r.be;
    for (auto& b : r.data) b = (uint8_t)rng();
    if (r.lock) r.rmw_pending = true;
  };

  for (int p = 0; p < kSpmPorts; p++) new_request(p);

  for (t = 0; t < args.cycles; t++) {
    // Phase schedule: uniform, hot bank (forces conflicts and promotions), streams
    uint64_t phase = (t / 5000) % 3;
    mode = phase == 0 ? Mode::Uniform : phase == 1 ? Mode::HotBank : Mode::Stream;
    if (t % 5000 == 0) hot_bank = (int)(rng() % kSpmBanks);

    // Drive requests
    uint32_t req = 0, we = 0, lock = 0, add = 0;
    for (int p = 0; p < kSpmPorts; p++) {
      const Req& r = port[p];
      if (r.valid) req |= 1u << p;
      if (r.we) we |= 1u << p;
      if (r.lock) lock |= 1u << p;
      if (r.add) add |= 1u << p;
      set_bits(dut->addr_i.data(), p * 16, 16, r.addr);
      set_bits(dut->be_i.data(), p * 16, 16, r.be);
      for (int b = 0; b < 16; b++) set_bits(dut->wdata_i.data(), p * 128 + b * 8, 8, r.data[b]);
    }
    dut->req_i = req;
    dut->we_i = we;
    dut->lock_i = lock;
    dut->add_i = add;
    tick(0);

    // Check responses from last cycle's grants before the clock edge: SRAM macros
    // drive read data only after the falling edge (tb_spm_sky130)
    for (int p = 0; p < kSpmPorts; p++) {
      bool rv = dut->rvalid_o >> p & 1;
      TB_CHECK(rv == exp_valid[p], "t=%llu port %d rvalid=%d expected %d",
               (unsigned long long)t, p, rv, exp_valid[p]);
      if (rv && exp_is_read[p]) {
        const uint32_t* w = &dut->rdata_o[p * 4];
        for (int b = 0; b < 16; b++) {
          uint8_t got = (uint8_t)(w[b / 4] >> (8 * (b % 4)));
          TB_CHECK(got == exp_data[p][b], "t=%llu port %d read byte %d: got %02x expected %02x",
                   (unsigned long long)t, p, b, got, exp_data[p][b]);
        }
        reads_checked++;
      }
    }

    // Model grants per bank
    uint32_t exp_gnt = 0, exp_prom = 0;
    for (int b = 0; b < kSpmBanks; b++) {
      uint32_t breq = 0, block = 0;
      for (int p = 0; p < kSpmPorts; p++)
        if ((req >> p & 1) && spm_bank_of(port[p].addr) == b) {
          breq |= 1u << p;
          if (lock >> p & 1) block |= 1u << p;
        }
      // lock_i reaches every bank's arbiter, as in the RTL
      uint32_t prom = 0;
      uint32_t g = arb[b].step(breq, lock, &prom);
      seen_masks[b].insert(breq);
      if (__builtin_popcount(breq) > 1) conflicts++;
      exp_gnt |= g;
      exp_prom |= prom;
      (void)block;
    }
    uint32_t gnt = dut->gnt_o, prom = dut->promoted_o, den = dut->denied_o;
    TB_CHECK(gnt == exp_gnt, "t=%llu grant mismatch: rtl %02x model %02x (req %02x lock %02x)",
             (unsigned long long)t, gnt, exp_gnt, req, lock);
    TB_CHECK(prom == exp_prom, "t=%llu promoted mismatch: rtl %02x model %02x",
             (unsigned long long)t, prom, exp_prom);
    TB_CHECK(den == (req & ~gnt), "t=%llu denied mismatch: %02x", (unsigned long long)t, den);

    // Apply granted accesses to the memory model
    for (int p = 0; p < kSpmPorts; p++) {
      exp_valid[p] = gnt >> p & 1;
      exp_is_read[p] = exp_valid[p] && !port[p].we;
      if (!exp_valid[p]) continue;
      if (port[p].we && port[p].add) {  // read word + increment, in the selected lane
        uint8_t cur[16], upd[16];
        mem.read128(port[p].addr, cur);
        int lane = (port[p].addr >> 2) & 3;
        uint32_t old, inc;
        memcpy(&old, &cur[4 * lane], 4);
        memcpy(&inc, &port[p].data[4 * lane], 4);
        old += inc;
        for (int l = 0; l < 4; l++) memcpy(&upd[4 * l], &old, 4);
        mem.write128(port[p].addr, upd, port[p].be);
        adds++;
      } else if (port[p].we) mem.write128(port[p].addr, port[p].data, port[p].be);
      else mem.read128(port[p].addr, exp_data[p]);
    }
    grants += __builtin_popcount(gnt);
    promotions += __builtin_popcount(prom);

    tick(1);

    // Advance masters
    for (int p = 0; p < kSpmPorts; p++) {
      if (gnt >> p & 1) {
        port[p].waited = 0;
        new_request(p);
      } else if (port[p].valid) {
        port[p].waited++;
        max_wait = std::max(max_wait, port[p].waited);
        TB_CHECK(port[p].waited <= arb[0].bound(), "t=%llu port %d waited %d > bound %d",
                 (unsigned long long)t, p, port[p].waited, arb[0].bound());
      } else {
        new_request(p);
      }
    }
    if (ctx->gotError()) return 1;
  }

  for (int b = 0; b < kSpmBanks; b++)
    TB_CHECK(seen_masks[b].size() == 128, "bank %d saw only %zu of 128 request patterns", b,
             seen_masks[b].size());

  tfp.close();
  dut->final();
  printf("tb_spm: PASS cycles=%llu grants=%llu adds=%llu reads_checked=%llu conflicts=%llu "
         "promotions=%llu max_wait=%d bound=%d\n",
         (unsigned long long)t, (unsigned long long)grants, (unsigned long long)adds, (unsigned long long)reads_checked,
         (unsigned long long)conflicts, (unsigned long long)promotions, max_wait, arb[0].bound());
  return 0;
}
