// Credit link unit test (SPEC.md, Verification strategy: link and credits).
//
// Random source and random receiver backpressure, then a full-rate window. A
// cycle-accurate model of the credit counter, output register, slices, input
// buffer and credit register must match the RTL every cycle: sender credits,
// link valid at the receiver, buffer occupancy and the head flit. Flits must
// arrive in order and unchanged. In the full-rate window the link must carry
// one flit per cycle when DEPTH >= the credit round trip, and exactly
// DEPTH / RTT of the cycles otherwise.
#include <verilated.h>
#include <verilated_fst_c.h>

#include <deque>
#include <memory>
#include <random>

#include "Vtb_link_top.h"
#include "tb_args.h"

#ifndef LINK_SLICE
#error "LINK_SLICE must be defined"
#endif
#ifndef LINK_DEPTH
#error "LINK_DEPTH must be defined"
#endif

namespace {

struct Flit {
  uint32_t w[7] = {};  // 204 bits
  bool operator==(const Flit& o) const { return memcmp(w, o.w, sizeof w) == 0; }
};

// Cycle model: state updated with the same edge semantics as the RTL.
class LinkModel {
 public:
  LinkModel(int slice, int depth) : slice_(slice), depth_(depth), credits_(depth) {
    fwd_.assign(slice, {false, {}});
    cr_.assign(slice, false);
  }
  // Combinational view of the current cycle
  int credits() const { return credits_; }
  bool rx_valid() const { return slice_ ? fwd_.back().first : out_valid_; }
  const Flit& rx_flit() const { return slice_ ? fwd_.back().second : out_flit_; }
  int count() const { return (int)fifo_.size(); }
  const Flit* head() const { return fifo_.empty() ? nullptr : &fifo_.front(); }
  bool credit_back() const { return slice_ ? cr_.back() : credit_q_; }

  // Clock edge with this cycle's inputs; returns whether the source flit was sent.
  bool edge(bool src_valid, const Flit& f, bool pop_req) {
    bool send = src_valid && credits_ > 0;
    bool pop = pop_req && !fifo_.empty();
    bool push = rx_valid();
    Flit pushed = rx_flit();
    bool cr_in = credit_back();
    // registers
    credits_ += (cr_in ? 1 : 0) - (send ? 1 : 0);
    if (slice_) {
      for (int i = slice_ - 1; i > 0; i--) {
        fwd_[i] = fwd_[i - 1];
        cr_[i] = cr_[i - 1];
      }
      fwd_[0] = {out_valid_, out_flit_};
      cr_[0] = credit_q_;
    }
    out_valid_ = send;
    if (send) out_flit_ = f;
    if (pop) fifo_.pop_front();
    if (push) fifo_.push_back(pushed);
    credit_q_ = pop;
    return send;
  }

 private:
  int slice_, depth_, credits_;
  bool out_valid_ = false;
  Flit out_flit_;
  std::deque<std::pair<bool, Flit>> fwd_;
  std::deque<bool> cr_;
  std::deque<Flit> fifo_;
  bool credit_q_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 100000);
  const int slice = LINK_SLICE, depth = LINK_DEPTH, rtt = 4 + 2 * slice;
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_link_top>(ctx.get());
  std::unique_ptr<VerilatedFstC> tfp;
  if (!args.trace.empty()) {
    ctx->traceEverOn(true);
    tfp = std::make_unique<VerilatedFstC>();
    dut->trace(tfp.get(), 99);
    tfp->open(args.trace.c_str());
  }
  auto half = [&](int clk) {
    dut->clk_i = clk;
    dut->eval();
    if (tfp) tfp->dump(ctx->time());
    ctx->timeInc(1);
  };

  ctx->assertOn(false);
  dut->rst_ni = 1;
  dut->src_valid_i = 0;
  dut->pop_i = 0;
  dut->eval();
  dut->rst_ni = 0;
  for (int i = 0; i < 3; i++) { half(0); half(1); }
  dut->rst_ni = 1;
  ctx->assertOn(true);

  LinkModel model(slice, depth);
  std::deque<Flit> expected;  // sent, not yet popped
  uint64_t seq = 0, sent = 0, popped = 0, bubbles = 0;
  const uint64_t random_cycles = args.cycles, window = 2000;
  uint64_t win_start = random_cycles + 200, win_end = win_start + window, win_rx = 0;

  for (uint64_t t = 0; t < win_end + 50; t++) {
    bool full_rate = t >= random_cycles;
    bool drain = t >= win_end;
    bool src_valid = drain ? false : full_rate ? true : (rng() % 100) < 70;
    bool pop_req = full_rate ? true : (rng() % 100) < 60;
    Flit f;
    for (int i = 0; i < 6; i++) f.w[i] = (uint32_t)rng();
    f.w[6] = (uint32_t)(seq & 0xfff);

    dut->src_valid_i = src_valid;
    for (int i = 0; i < 7; i++) dut->src_flit_i[i] = f.w[i];
    dut->pop_i = pop_req;
    half(0);

    // Compare the combinational view of this cycle
    TB_CHECK(dut->credits_o == model.credits(), "t=%llu credits rtl %d model %d",
             (unsigned long long)t, dut->credits_o, model.credits());
    TB_CHECK(dut->rx_valid_o == model.rx_valid(), "t=%llu rx_valid rtl %d model %d",
             (unsigned long long)t, dut->rx_valid_o, model.rx_valid());
    TB_CHECK(dut->rx_count_o == model.count(), "t=%llu count rtl %d model %d",
             (unsigned long long)t, dut->rx_count_o, model.count());
    if (model.head()) {
      Flit h;
      for (int i = 0; i < 7; i++) h.w[i] = dut->rx_head_o[i];
      h.w[6] &= 0xfff;
      Flit m = *model.head();
      m.w[6] &= 0xfff;
      TB_CHECK(h == m, "t=%llu head flit differs from the model", (unsigned long long)t);
      TB_CHECK(!expected.empty() && h == expected.front(), "t=%llu head flit out of order",
               (unsigned long long)t);
    }
    bool taken = dut->src_taken_o;
    if (dut->bubble_o) bubbles++;
    bool pop = pop_req && model.count() > 0;
    if (pop) {
      expected.pop_front();
      popped++;
    }
    if (dut->rx_valid_o && t >= win_start && t < win_end) win_rx++;

    bool msent = model.edge(src_valid, f, pop_req);
    TB_CHECK(taken == msent, "t=%llu send rtl %d model %d", (unsigned long long)t, taken, msent);
    if (taken) {
      Flit e = f;
      e.w[6] &= 0xfff;
      expected.push_back(e);
      sent++;
      seq++;
    }
    half(1);
    if (ctx->gotError()) return 1;
  }

  // Idle at the end: every credit is back
  for (int i = 0; i < rtt + 4; i++) {
    dut->src_valid_i = 0;
    dut->pop_i = 1;
    half(0);
    if (dut->rx_count_o) {
      expected.pop_front();
      popped++;
    }
    model.edge(false, Flit{}, true);
    half(1);
  }
  half(0);
  TB_CHECK(dut->credits_o == depth, "credits after drain %d, expected %d (lost credit)",
           dut->credits_o, depth);
  TB_CHECK(sent == popped && expected.empty(), "sent %llu popped %llu",
           (unsigned long long)sent, (unsigned long long)popped);
  TB_CHECK(bubbles == 0, "%llu bubbles", (unsigned long long)bubbles);

  // Full-rate window: one flit per cycle at DEPTH >= RTT, DEPTH/RTT otherwise
  double rate = (double)win_rx / window;
  double want = depth >= rtt ? 1.0 : (double)depth / rtt;
  TB_CHECK(rate > want - 0.002 && rate < want + 0.002, "full-rate window: %.4f flits/cycle, expected %.4f",
           rate, want);
  if (tfp) tfp->close();
  printf("tb_link: PASS slice=%d depth=%d rtt=%d sent=%llu full-rate=%.3f flits/cycle\n", slice,
         depth, rtt, (unsigned long long)sent, rate);
  return 0;
}
