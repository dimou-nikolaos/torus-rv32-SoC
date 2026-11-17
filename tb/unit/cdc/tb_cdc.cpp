// Async FIFO test (SPEC.md, I/O tile verification): two free-running clocks
// whose periods and phase change every phase (ratios from 1:1 to 1:9 either
// way, near-equal clocks that drift past each other, and per-edge jitter),
// random push and pop rates. Every value comes out exactly once and in order;
// the RTL asserts no push while full and no pop while empty. With both sides
// always willing, the throughput must reach the slower clock's rate.
#include <verilated.h>
#include <verilated_fst_c.h>

#include <memory>
#include <random>

#include "Vtb_cdc_top.h"
#include "tb_args.h"

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 400000);
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randReset(2);
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_cdc_top>(ctx.get());
  std::unique_ptr<VerilatedFstC> tfp;
  if (!args.trace.empty()) {
    ctx->traceEverOn(true);
    tfp = std::make_unique<VerilatedFstC>();
    dut->trace(tfp.get(), 99);
    tfp->open(args.trace.c_str());
  }

  // Clocks: half periods in time units, next edge times
  uint64_t now = 0;
  int wh = 5, rh = 5, jitter = 0;
  uint64_t wnext = 3, rnext = 7;
  int push_pct = 50, pop_pct = 50;
  uint32_t wseq = 0, rseq = 0;
  uint64_t wedges = 0, redges = 0, pushes = 0, pops = 0;

  auto eval = [&]() {
    dut->eval();
    if (tfp) tfp->dump(now);
  };

  ctx->assertOn(false);
  dut->wclk_i = 0;
  dut->rclk_i = 0;
  dut->push_i = 0;
  dut->pop_i = 0;
  dut->wrst_ni = 1;
  dut->rrst_ni = 1;
  eval();
  dut->wrst_ni = 0;
  dut->rrst_ni = 0;
  eval();
  dut->wrst_ni = 1;
  dut->rrst_ni = 1;
  eval();
  ctx->assertOn(true);

  // One step: advance to the next clock edge(s); on a rising edge, decide the
  // inputs of that domain from its outputs (which only change on its own edges)
  auto step = [&]() -> int {
    uint64_t t = std::min(wnext, rnext);
    now = t;
    bool w = wnext == t, r = rnext == t;
    if (w && !dut->wclk_i) {
      dut->push_i = !dut->full_o && (int)(rng() % 100) < push_pct;
      dut->data_i = wseq;
    }
    if (r && !dut->rclk_i) {
      dut->pop_i = !dut->empty_o && (int)(rng() % 100) < pop_pct;
      if (dut->pop_i) {
        TB_CHECK(dut->head_o == rseq, "t=%llu popped %u, expected %u", (unsigned long long)now, dut->head_o, rseq);
      }
    }
    eval();
    if (w) {
      if (!dut->wclk_i) {
        if (dut->push_i) { wseq++; pushes++; }
        wedges++;
      }
      dut->wclk_i = !dut->wclk_i;
      wnext += (uint64_t)(wh + (jitter ? (int)(rng() % (2 * jitter + 1)) - jitter : 0));
    }
    if (r) {
      if (!dut->rclk_i) {
        if (dut->pop_i) { rseq++; pops++; }
        redges++;
      }
      dut->rclk_i = !dut->rclk_i;
      rnext += (uint64_t)(rh + (jitter ? (int)(rng() % (2 * jitter + 1)) - jitter : 0));
    }
    eval();
    return ctx->gotError() ? 1 : 0;
  };

  // Phases: (write half period, read half period, jitter)
  struct Ratio { int wh, rh, jitter; };
  const Ratio ratios[] = {{5, 5, 0}, {5, 45, 0}, {45, 5, 0}, {50, 51, 0}, {51, 50, 0}, {7, 13, 0},
                          {13, 7, 0}, {20, 20, 6}, {6, 30, 2}, {30, 6, 2}};
  uint64_t steps_per_phase = args.cycles / 40;
  int phase = 0;
  for (uint64_t done = 0; done < args.cycles; phase++) {
    const Ratio& rt = ratios[rng() % (sizeof ratios / sizeof ratios[0])];
    wh = rt.wh;
    rh = rt.rh;
    jitter = rt.jitter;
    // A new phase offset: delay one clock by a random fraction of its period
    if (rng() & 1) wnext += rng() % (uint64_t)wh; else rnext += rng() % (uint64_t)rh;
    bool full_rate = phase % 3 == 2;
    push_pct = full_rate ? 100 : (int)(rng() % 100) + 1;
    pop_pct = full_rate ? 100 : (int)(rng() % 100) + 1;
    uint64_t w0 = wedges, r0 = redges, p0 = pops;
    for (uint64_t i = 0; i < steps_per_phase; i++, done++)
      if (step()) return 1;
    if (full_rate) {
      // Items per edge of the slower clock (after the first phase's fill)
      double slow = (double)std::min(wedges - w0, redges - r0);
      double rate = (pops - p0) / slow;
      if (!args.quiet)
        printf("phase %d: half periods w=%d r=%d jitter %d: %.3f items per slower-clock cycle\n", phase, wh, rh,
               jitter, rate);
      TB_CHECK(rate > 0.95, "phase %d (w=%d r=%d): throughput %.3f of the slower clock", phase, wh, rh, rate);
    }
  }
  // Drain
  push_pct = 0;
  pop_pct = 100;
  for (int i = 0; i < 10000 && rseq != wseq; i++)
    if (step()) return 1;
  TB_CHECK(rseq == wseq && dut->empty_o, "drain: pushed %u, popped %u", wseq, rseq);
  if (tfp) tfp->close();
  dut->final();
  printf("tb_cdc: PASS %u values, %d phases, %llu write and %llu read cycles\n", wseq, phase,
         (unsigned long long)wedges, (unsigned long long)redges);
  return 0;
}
