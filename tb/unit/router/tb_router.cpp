// Single-router test (SPEC.md, Verification strategy: router; NoC full-throughput
// rule). The router sits at (CX, CY) in a GX x GY plane with a full link on each
// port (tb_router_top). A cycle model of the whole assembly must match the RTL
// every cycle: upstream credits, router outputs, downstream buffers and all
// router events. On top of that:
//   - every flit leaves exactly once, on the port its route names, with the
//     right lookahead port for the next router, in order per input/output pair;
//   - per-hop latency: input link to output link in exactly 2 cycles;
//   - throughput: every legal pass-through pair, all inputs to distinct outputs
//     at once, and recovery after a downstream stall run at one flit per cycle;
//   - bubble rule: flits continuing on a ring are sent with one credit left,
//     entering flits never are, and the hold is counted;
//   - zero bubbles in every phase.
#include <verilated.h>

#include <map>
#include <memory>
#include <random>
#include <vector>

#include "Vtb_router_top.h"
#include "router_model.h"
#include "tb_args.h"
#include "tb_trace.h"

using namespace torus;

#ifndef R_DIR
#define R_DIR 0
#endif
#ifndef R_SLICE
#define R_SLICE 0
#endif

namespace {

constexpr int kGX = 4, kGY = 4, kCX = 1, kCY = 2;
constexpr int kDir = R_DIR, kSlice = R_SLICE, kDepth = 5 + 2 * R_SLICE;
const char* kPortName[] = {"L", "X", "Y"};

// The whole tb_router_top as a cycle model
class Assembly {
 public:
  Assembly() : r(kDir, kGX, kGY, kDepth, kCX, kCY) {
    for (int p = 0; p < kPorts; p++) {
      up_cred[p] = kDepth;
      in_fwd[p] = Delay<LinkFwd>(kSlice);
      in_cr[p] = Delay<bool>(kSlice);
      out_fwd[p] = Delay<LinkFwd>(kSlice);
      out_cr[p] = Delay<bool>(kSlice);
    }
  }
  const LinkFwd& r_in(int p) const { return kSlice ? in_fwd[p].out() : up_q[p]; }
  bool up_credit_back(int p) const { return kSlice ? in_cr[p].out() : r.credit_out(p); }
  const LinkFwd& dn_in(int p) const { return kSlice ? out_fwd[p].out() : r.out(p); }
  bool r_credit_in(int p) const { return kSlice ? out_cr[p].out() : dn_credit_q[p]; }

  // Edge with this cycle's inputs; returns which sources were taken.
  std::array<bool, kPorts> edge(const std::array<bool, kPorts>& src_valid,
                                const std::array<LinkFwd, kPorts>& src,
                                const std::array<bool, kPorts>& pop_req) {
    std::array<bool, kPorts> taken{}, pop{};
    std::array<LinkFwd, kPorts> rin, dnin;
    std::array<bool, kPorts> crin, upcr;
    for (int p = 0; p < kPorts; p++) {
      taken[p] = src_valid[p] && up_cred[p] > 0;
      pop[p] = pop_req[p] && !dn[p].empty();
      rin[p] = r_in(p);
      dnin[p] = dn_in(p);
      crin[p] = r_credit_in(p);
      upcr[p] = up_credit_back(p);
    }
    std::array<bool, kPorts> r_cr_out;
    for (int p = 0; p < kPorts; p++) r_cr_out[p] = r.credit_out(p);
    std::array<LinkFwd, kPorts> r_out;
    for (int p = 0; p < kPorts; p++) r_out[p] = r.out(p);
    r.edge(rin, crin);
    for (int p = 0; p < kPorts; p++) {
      up_cred[p] += (upcr[p] ? 1 : 0) - (taken[p] ? 1 : 0);
      in_fwd[p].edge(up_q[p]);
      in_cr[p].edge(r_cr_out[p]);
      out_fwd[p].edge(r_out[p]);
      out_cr[p].edge(dn_credit_q[p]);
      up_q[p].valid = taken[p];
      if (taken[p]) {
        up_q[p].la = src[p].la;
        up_q[p].flit = src[p].flit;
      }
      if (pop[p]) dn[p].pop_front();
      if (dnin[p].valid) dn[p].push_back({dnin[p].la, dnin[p].flit});
      dn_credit_q[p] = pop[p];
    }
    return taken;
  }

  RouterModel r;
  std::array<int, kPorts> up_cred;
  std::array<LinkFwd, kPorts> up_q{};
  std::array<Delay<LinkFwd>, kPorts> in_fwd, out_fwd;
  std::array<Delay<bool>, kPorts> in_cr, out_cr;
  std::array<std::deque<Entry>, kPorts> dn;
  std::array<bool, kPorts> dn_credit_q{};
};

struct Sent {
  uint32_t id;
  int in;
  int out;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 60000);
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_router_top>(ctx.get());
  TbTrace tfp;
  tfp.open(ctx.get(), dut.get(), args.trace);
  auto half = [&](int clk) {
    dut->clk_i = clk;
    dut->eval();
    tfp.dump(ctx->time());
    ctx->timeInc(1);
  };

  dut->x_i = kCX;
  dut->y_i = kCY;
  ctx->assertOn(false);
  dut->rst_ni = 1;
  dut->src_valid_i = 0;
  dut->pop_i = 0;
  dut->eval();
  dut->rst_ni = 0;
  for (int i = 0; i < 3; i++) { half(0); half(1); }
  dut->rst_ni = 1;
  ctx->assertOn(true);

  Assembly m;
  uint32_t next_id = 1;
  std::map<uint32_t, Sent> outstanding;
  std::map<std::pair<int, int>, uint32_t> last_id;  // order per (in, out)
  uint64_t delivered = 0, bubble_holds = 0, ring_sends_at_one = 0, total_bubbles = 0;
  uint64_t t = 0;

  // Destinations: an input-legal destination whose route here is `want` (or random)
  auto make_dst = [&](int in, int want, uint8_t* dx, uint8_t* dy) {
    for (int tries = 0; tries < 1000; tries++) {
      uint8_t x = (uint8_t)(rng() % kGX), y = (uint8_t)(rng() % kGY);
      if (in == kY) x = kCX;  // already on its Y ring: X done
      int r = (int)route(kCX, kCY, x, y);
      if (want < 0 || r == want) {
        *dx = x;
        *dy = y;
        return true;
      }
    }
    return false;
  };

  // Drive one cycle. src_req[p] = desired output for input p (-2 none, -1 random).
  struct Window {
    bool on = false;
    std::array<uint64_t, kPorts> out_flits{};
    uint64_t cycles = 0;
  } win;
  std::array<int, kPorts> pending_out;
  pending_out.fill(-2);
  std::array<LinkFwd, kPorts> pending{};
  std::array<bool, kPorts> have{};
  int hop_latency = -1;

  auto cycle = [&](const std::array<int, kPorts>& src_req, const std::array<bool, kPorts>& pop_req) -> int {
    std::array<bool, kPorts> src_valid{};
    for (int p = 0; p < kPorts; p++) {
      if (!have[p] && src_req[p] != -2) {
        uint8_t dx, dy;
        if (make_dst(p, src_req[p], &dx, &dy)) {
          Flit f;
          f.dst_x = dx;
          f.dst_y = dy;
          f.src_x = (uint8_t)(rng() & 15);
          f.src_y = (uint8_t)(rng() & 15);
          f.op = Op::Write;
          f.addr = (uint32_t)rng();
          f.be = (uint16_t)rng();
          f.tag = (uint8_t)p;
          f.data[0] = next_id++;
          for (int k = 1; k < 4; k++) f.data[k] = (uint32_t)rng();
          pending[p].flit = f;
          pending[p].la = route(kCX, kCY, dx, dy);
          pending[p].valid = true;
          have[p] = true;
        }
      }
      src_valid[p] = have[p];
    }
    // Drive
    uint8_t sv = 0, pv = 0;
    for (int p = 0; p < kPorts; p++) {
      if (src_valid[p]) sv |= 1 << p;
      if (pop_req[p]) pv |= 1 << p;
      uint32_t w[Flit::kWords];
      pending[p].flit.pack(w);
      for (int k = 0; k < Flit::kWords; k++) {
        // src_flit_i is a packed array of 3 flits: port p at bits [p*204 +: 204]
        for (int b = 0; b < 32; b++) {
          int bit = p * Flit::kBits + k * 32 + b;
          if (k * 32 + b >= Flit::kBits) break;
          uint32_t& word = dut->src_flit_i[bit / 32];
          if (w[k] >> b & 1) word |= 1u << (bit % 32);
          else word &= ~(1u << (bit % 32));
        }
      }
    }
    dut->src_valid_i = sv;
    dut->src_la_i = (uint8_t)((int)pending[0].la | (int)pending[1].la << 2 | (int)pending[2].la << 4);
    dut->pop_i = pv;
    half(0);

    // Compare the current cycle with the model
    RouterEvents ev;
    m.r.allocate(&ev);
    for (int p = 0; p < kPorts; p++) {
      TB_CHECK(((dut->up_credits_o >> (4 * p)) & 15) == (uint32_t)m.up_cred[p],
               "t=%llu up credits[%s] rtl %u model %d", (unsigned long long)t, kPortName[p],
               (dut->up_credits_o >> (4 * p)) & 15, m.up_cred[p]);
      TB_CHECK(((dut->out_valid_o >> p) & 1) == m.r.out(p).valid, "t=%llu out_valid[%s]",
               (unsigned long long)t, kPortName[p]);
      TB_CHECK(((dut->rx_count_o >> (4 * p)) & 15) == m.dn[p].size(), "t=%llu dn count[%s] rtl %u model %zu",
               (unsigned long long)t, kPortName[p], (dut->rx_count_o >> (4 * p)) & 15, m.dn[p].size());
      TB_CHECK(((dut->ev_flit_o >> p) & 1) == ev.flit[p], "t=%llu ev_flit[%s]", (unsigned long long)t, kPortName[p]);
      TB_CHECK(((dut->ev_credit_stall_o >> p) & 1) == ev.credit_stall[p], "t=%llu ev_credit_stall[%s]",
               (unsigned long long)t, kPortName[p]);
      TB_CHECK(((dut->ev_bubble_hold_o >> p) & 1) == ev.bubble_hold[p], "t=%llu ev_bubble_hold[%s]",
               (unsigned long long)t, kPortName[p]);
      TB_CHECK(((dut->ev_arb_loss_o >> p) & 1) == ev.arb_loss[p], "t=%llu ev_arb_loss[%s]",
               (unsigned long long)t, kPortName[p]);
      TB_CHECK(!((dut->ev_bubble_o >> p) & 1) && !ev.bubble[p], "t=%llu bubble on %s", (unsigned long long)t,
               kPortName[p]);
      bubble_holds += ev.bubble_hold[p];
    }
    // Bubble rule observed at grant time
    auto g = m.r.allocate();
    for (int o = 0; o < kPorts; o++) {
      if (g[o] < 0) continue;
      if (RouterModel::entering(g[o], o))
        TB_CHECK(m.r.credits(o) >= 2, "t=%llu entering flit %s->%s sent with %d credits", (unsigned long long)t,
                 kPortName[g[o]], kPortName[o], m.r.credits(o));
      else if (o != kL && m.r.credits(o) == 1)
        ring_sends_at_one++;
    }
    // Deliveries: heads popped this cycle
    for (int p = 0; p < kPorts; p++) {
      if (!pop_req[p] || m.dn[p].empty()) continue;
      const Entry& e = m.dn[p].front();
      uint32_t w[Flit::kWords];
      for (int k = 0; k < Flit::kWords; k++) w[k] = 0;
      for (int b = 0; b < Flit::kBits; b++) {
        int bit = p * Flit::kBits + b;
        if (dut->rx_head_o[bit / 32] >> (bit % 32) & 1) w[b / 32] |= 1u << (b % 32);
      }
      Flit rf = Flit::unpack(w);
      int rla = (dut->rx_head_la_o >> (2 * p)) & 3;
      TB_CHECK(rf == e.flit && rla == (int)e.la, "t=%llu output %s head differs from the model: rtl %s la %d, model %s la %d",
               (unsigned long long)t, kPortName[p], rf.str().c_str(), rla, e.flit.str().c_str(), (int)e.la);
      uint32_t id = rf.data[0];
      auto it = outstanding.find(id);
      TB_CHECK(it != outstanding.end(), "t=%llu flit %u delivered twice or never sent", (unsigned long long)t, id);
      TB_CHECK(it->second.out == p, "t=%llu flit %u left on %s, route says %s", (unsigned long long)t, id,
               kPortName[p], kPortName[it->second.out]);
      // Lookahead port for the next router on this output's ring
      int nx = kCX, ny = kCY;
      if (p == kX) nx = ring_next(kCX, kGX, kDir);
      if (p == kY) ny = ring_next(kCY, kGY, kDir);
      if (p != kL)
        TB_CHECK(rla == (int)route(nx, ny, rf.dst_x, rf.dst_y), "t=%llu flit %u lookahead %d wrong",
                 (unsigned long long)t, id, rla);
      auto key = std::make_pair(it->second.in, p);
      TB_CHECK(id > last_id[key], "t=%llu flit %u out of order on %s->%s", (unsigned long long)t, id,
               kPortName[it->second.in], kPortName[p]);
      last_id[key] = id;
      outstanding.erase(it);
      delivered++;
    }
    if (win.on) {
      win.cycles++;
      for (int p = 0; p < kPorts; p++) win.out_flits[p] += ev.flit[p];
    }
    // Per-hop latency: first flit into an empty router
    if (hop_latency == -2 && m.r_in(kL).valid) hop_latency = -(int)(t + 100);  // mark arrival
    if (hop_latency < -2 && m.r.out(kX).valid) hop_latency = (int)t - (-hop_latency - 100);

    auto taken = m.edge(src_valid, pending, pop_req);
    for (int p = 0; p < kPorts; p++) {
      TB_CHECK(((dut->src_taken_o >> p) & 1) == taken[p], "t=%llu taken[%s]", (unsigned long long)t, kPortName[p]);
      if (taken[p]) {
        int out = (int)pending[p].la;
        outstanding[pending[p].flit.data[0]] = {pending[p].flit.data[0], p, out};
        have[p] = false;
      }
    }
    half(1);
    t++;
    total_bubbles += 0;
    return ctx->gotError() ? 1 : 0;
  };

  auto drain = [&]() -> int {
    for (int i = 0; i < 200; i++)
      if (cycle({-2, -2, -2}, {true, true, true})) return 1;
    return 0;
  };
  auto rate_ok = [&](int o, const char* what) -> bool {
    double r = (double)win.out_flits[o] / win.cycles;
    if (r < 0.999) {
      fprintf(stderr, "%s: output %s at %.3f flits/cycle\n", what, kPortName[o], r);
      return false;
    }
    return true;
  };

  // 1. Per-hop latency: one flit L -> X into an empty router
  hop_latency = -2;
  if (cycle({kX, -2, -2}, {true, true, true})) return 1;
  for (int i = 0; i < 20; i++) if (cycle({-2, -2, -2}, {true, true, true})) return 1;
  TB_CHECK(hop_latency == 2, "per-hop latency %d cycles, expected 2", hop_latency);

  // 2. Random traffic with random backpressure
  for (uint64_t i = 0; i < args.cycles; i++) {
    std::array<int, kPorts> req;
    std::array<bool, kPorts> pop;
    for (int p = 0; p < kPorts; p++) {
      req[p] = (rng() % 100) < 75 ? -1 : -2;
      pop[p] = (rng() % 100) < ((i / 3000) % 2 ? 30 : 80);
    }
    if (cycle(req, pop)) return 1;
  }
  if (drain()) return 1;

  // 3. Pass-through on every legal input -> output pair at full rate
  const int pairs[][2] = {{kL, kX}, {kL, kY}, {kL, kL}, {kX, kX}, {kX, kY}, {kX, kL}, {kY, kY}, {kY, kL}};
  for (auto& pr : pairs) {
    std::array<int, kPorts> req{-2, -2, -2};
    req[pr[0]] = pr[1];
    for (int i = 0; i < 100; i++) if (cycle(req, {true, true, true})) return 1;
    win = Window{};
    win.on = true;
    for (int i = 0; i < 1000; i++) if (cycle(req, {true, true, true})) return 1;
    win.on = false;
    if (!rate_ok(pr[1], "pass-through")) return 1;
    if (drain()) return 1;
  }

  // 4. All inputs to distinct outputs at once: L -> X, X -> Y, Y -> L
  {
    std::array<int, kPorts> req{kX, kY, kL};
    for (int i = 0; i < 100; i++) if (cycle(req, {true, true, true})) return 1;
    win = Window{};
    win.on = true;
    for (int i = 0; i < 1000; i++) if (cycle(req, {true, true, true})) return 1;
    win.on = false;
    for (int o = 0; o < kPorts; o++) if (!rate_ok(o, "parallel")) return 1;
    if (drain()) return 1;
  }

  // 5. Recovery: X -> X stream, downstream stalls 50 cycles, then full rate within one round trip
  {
    std::array<int, kPorts> req{-2, kX, -2};
    for (int i = 0; i < 100; i++) if (cycle(req, {true, true, true})) return 1;
    for (int i = 0; i < 50; i++) if (cycle(req, {true, false, true})) return 1;
    for (int i = 0; i < credit_rtt(kSlice) + 2; i++) if (cycle(req, {true, true, true})) return 1;
    win = Window{};
    win.on = true;
    for (int i = 0; i < 500; i++) if (cycle(req, {true, true, true})) return 1;
    win.on = false;
    if (!rate_ok(kX, "recovery")) return 1;
    if (drain()) return 1;
  }

  // 6. Bubble rule: X output stalled with both a continuing (X -> X) and an
  //    entering (L -> X) stream. The continuing stream must use the last credit.
  {
    uint64_t before = ring_sends_at_one, holds = bubble_holds;
    std::array<int, kPorts> req{kX, kX, -2};
    for (int i = 0; i < 2000; i++)
      if (cycle(req, {true, (rng() % 100) < 15, true})) return 1;
    TB_CHECK(ring_sends_at_one > before, "no continuing flit was ever sent with one credit left");
    TB_CHECK(bubble_holds > holds, "the bubble rule never held an entering flit");
    if (drain()) return 1;
  }

  TB_CHECK(outstanding.empty(), "%zu flits never delivered", outstanding.size());
  tfp.close();
  printf("tb_router: PASS dir=%d slice=%d depth=%d delivered=%llu hop=2 bubble_holds=%llu "
         "ring_sends_at_one_credit=%llu\n",
         kDir, kSlice, kDepth, (unsigned long long)delivered, (unsigned long long)bubble_holds,
         (unsigned long long)ring_sends_at_one);
  return 0;
}
