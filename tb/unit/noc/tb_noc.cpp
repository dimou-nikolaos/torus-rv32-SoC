// Bare-plane NoC test (SPEC.md, Verification strategy: NoC; full-throughput rule).
//
// Endpoints at every node inject and eject; the transaction-level model gives
// each flit's path. Checks, in every phase:
//   - exactly-once delivery at the right node, in order per (source, destination);
//   - per-link flit counts equal the model's exactly at the end;
//   - zero bubbles; the deadlock watchdog never fires (no flit moves anywhere for
//     WATCHDOG cycles while flits are in flight -> dump and fail).
// Phases: zero-load latency for every source/destination pair (exact), uniform
// random with backpressure, transpose, hotspot, neighbor permutation on X and
// on Y (every ring link at 100%), a stream across the diameter (1 flit/cycle),
// and a saturating soak with heavy backpressure. --soak=N sets the soak length;
// --expect-deadlock inverts the result for the BUBBLE=0 build.
#include <verilated.h>
#include <verilated_fst_c.h>

#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include "Vtb_noc_top.h"
#include "noc_tlm.h"
#include "tb_args.h"

using namespace torus;

#ifndef N_GX
#define N_GX 2
#endif
#ifndef N_GY
#define N_GY 2
#endif
#ifndef N_DIR
#define N_DIR 0
#endif
#ifndef N_SLICE
#define N_SLICE 0
#endif

namespace {
constexpr int kGX = N_GX, kGY = N_GY, kN = kGX * kGY, kDir = N_DIR, kSlice = N_SLICE;
constexpr uint64_t kWatchdog = 2000;
const char* kPortName[] = {"L", "X", "Y"};

template <typename W>
void put_flit(W& vec, int n, const Flit& f) {
  uint32_t w[Flit::kWords];
  f.pack(w);
  for (int b = 0; b < Flit::kBits; b++) {
    int bit = n * Flit::kBits + b;
    uint32_t& word = vec[bit / 32];
    if (w[b / 32] >> (b % 32) & 1) word |= 1u << (bit % 32);
    else word &= ~(1u << (bit % 32));
  }
}

template <typename W>
Flit get_flit(const W& vec, int n) {
  uint32_t w[Flit::kWords] = {};
  for (int b = 0; b < Flit::kBits; b++) {
    int bit = n * Flit::kBits + b;
    if (vec[bit / 32] >> (bit % 32) & 1) w[b / 32] |= 1u << (b % 32);
  }
  return Flit::unpack(w);
}

template <typename W>
bool get_bit(const W& vec, int bit) { return vec[bit / 32] >> (bit % 32) & 1; }
inline bool get_bit(uint64_t v, int bit) { return v >> bit & 1; }
inline bool get_bit(uint32_t v, int bit) { return v >> bit & 1; }
inline bool get_bit(uint16_t v, int bit) { return v >> bit & 1; }
inline bool get_bit(uint8_t v, int bit) { return v >> bit & 1; }

template <typename W>
void set_bit(W& vec, int bit, bool v) {
  if (v) vec[bit / 32] |= 1u << (bit % 32);
  else vec[bit / 32] &= ~(1u << (bit % 32));
}
template <typename T>
void set_scalar_bit(T& v, int bit, bool b) {
  if (b) v |= (T)1 << bit;
  else v &= ~((T)1 << bit);
}

struct Inflight {
  int src, dst;
  uint64_t sent_at;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 20000);
  uint64_t soak = std::stoull(args.opt("soak", "100000"));
  bool expect_deadlock = args.flag("expect-deadlock");
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_noc_top>(ctx.get());
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

  NocTlm tlm(kGX, kGY, kDir, kSlice);
  std::map<uint32_t, Inflight> inflight;
  std::map<std::pair<int, int>, uint32_t> last_id;
  std::vector<std::vector<uint64_t>> rtl_links(kN, std::vector<uint64_t>(kPorts, 0));
  std::vector<Flit> pending(kN);
  std::vector<bool> have(kN, false);
  uint32_t next_id = 1;
  uint64_t t = 0, idle = 0, delivered = 0, holds = 0;
  int deadlocked = 0;

  // Per-cycle driver. dst_of(n) returns the destination node for a new flit at
  // n, or -1 for none; pop_p is the probability of popping a waiting flit.
  // Optional hooks observe each delivery.
  std::vector<uint64_t> win_links(kN * kPorts, 0);
  bool win_on = false;
  uint64_t win_cycles = 0, win_ejected = 0;
  int latency_probe_id = -1;
  int64_t latency_seen = -1;

  auto cycle = [&](auto dst_of, int pop_pct) -> int {
    for (int n = 0; n < kN; n++) {
      if (!have[n]) {
        int d = dst_of(n);
        if (d >= 0) {
          Flit f;
          f.src_x = (uint8_t)(n % kGX);
          f.src_y = (uint8_t)(n / kGX);
          f.dst_x = (uint8_t)(d % kGX);
          f.dst_y = (uint8_t)(d / kGX);
          f.op = Op::Write;
          f.addr = (uint32_t)rng();
          f.be = 0xffff;
          f.data[0] = next_id++;
          f.data[1] = (uint32_t)rng();
          pending[n] = f;
          have[n] = true;
        }
      }
      set_scalar_bit(dut->src_valid_i, n, have[n]);
      put_flit(dut->src_flit_i, n, pending[n]);
      bool pop = (int)(rng() % 100) < pop_pct;
      set_scalar_bit(dut->pop_i, n, pop);
    }
    half(0);

    bool progress = false;
    // Router events
    for (int n = 0; n < kN; n++)
      for (int p = 0; p < kPorts; p++) {
        int bit = n * kPorts + p;
        if (get_bit(dut->ev_flit_o, bit)) {
          rtl_links[n][p]++;
          progress = true;
          if (win_on) win_links[bit]++;
        }
        TB_CHECK(!get_bit(dut->ev_bubble_o, bit), "t=%llu bubble at node %d port %s", (unsigned long long)t, n,
                 kPortName[p]);
        holds += get_bit(dut->ev_bubble_hold_o, bit);
      }
    // Deliveries
    for (int n = 0; n < kN; n++) {
      if (!get_bit(dut->pop_i, n) || !get_bit(dut->rx_valid_o, n)) continue;
      Flit f = get_flit(dut->rx_head_o, n);
      uint32_t id = f.data[0];
      auto it = inflight.find(id);
      TB_CHECK(it != inflight.end(), "t=%llu node %d: flit %u delivered twice or never sent", (unsigned long long)t,
               n, id);
      TB_CHECK(it->second.dst == n, "t=%llu flit %u for node %d ejected at node %d", (unsigned long long)t, id,
               it->second.dst, n);
      auto key = std::make_pair(it->second.src, n);
      TB_CHECK(id > last_id[key], "t=%llu flit %u out of order on %d->%d", (unsigned long long)t, id,
               it->second.src, n);
      last_id[key] = id;
      if ((int)id == latency_probe_id) latency_seen = (int64_t)(t - it->second.sent_at);
      tlm.deliver(it->second.src % kGX, it->second.src / kGX, n % kGX, n / kGX);
      inflight.erase(it);
      delivered++;
      progress = true;
      if (win_on) win_ejected++;
    }
    // Sends
    for (int n = 0; n < kN; n++)
      if (get_bit(dut->src_taken_o, n)) {
        inflight[pending[n].data[0]] = {n, pending[n].dst_y * kGX + pending[n].dst_x, t};
        have[n] = false;
        progress = true;
      }
    half(1);
    t++;
    if (win_on) win_cycles++;
    idle = (progress || inflight.empty()) ? 0 : idle + 1;
    if (idle > kWatchdog) {
      deadlocked = 1;
      fprintf(stderr, "WATCHDOG: no flit moved for %llu cycles with %zu in flight (t=%llu)\n",
              (unsigned long long)kWatchdog, inflight.size(), (unsigned long long)t);
      std::map<int, int> per_dst;
      for (auto& [id, f] : inflight) per_dst[f.dst]++;
      for (auto& [d, c] : per_dst) fprintf(stderr, "  %d flits in flight to node %d\n", c, d);
      for (int n = 0; n < kN; n++)
        for (int p = 0; p < kPorts; p++)
          if (get_bit(dut->ev_credit_stall_o, n * kPorts + p))
            fprintf(stderr, "  node %d (%d,%d) output %s: waiting, no credit\n", n, n % kGX, n / kGX, kPortName[p]);
      return 2;
    }
    return ctx->gotError() ? 1 : 0;
  };

  auto none = [](int) { return -1; };
  auto drain = [&]() -> int {
    for (int i = 0; i < 2000 && !inflight.empty(); i++) {
      int r = cycle(none, 100);
      if (r) return r;
    }
    for (int n = 0; n < kN; n++) if (have[n]) return 0;  // pending sources still drain next phase
    return inflight.empty() ? 0 : 3;
  };
  auto finish = [&](int r) -> int {
    if (expect_deadlock) {
      if (r == 2) {
        printf("tb_noc: PASS (expected deadlock without the bubble rule was detected)\n");
        return 0;
      }
      fprintf(stderr, "expected a deadlock without the bubble rule, got none (r=%d)\n", r);
      return 1;
    }
    return r;
  };
#define RUN(expr)            \
  do {                       \
    int _r = (expr);         \
    if (_r) return finish(_r); \
  } while (0)

  // 1. Zero-load latency, every source/destination pair, exact
  if (!expect_deadlock) {
    for (int s = 0; s < kN; s++)
      for (int d = 0; d < kN; d++) {
        bool sent = false;
        latency_probe_id = (int)next_id;
        latency_seen = -1;
        auto one = [&](int n) { if (n == s && !sent) { sent = true; return d; } return -1; };
        RUN(cycle(one, 100));
        for (int i = 0; i < 200 && latency_seen < 0; i++) RUN(cycle(none, 100));
        int want = tlm.zero_load_latency(s % kGX, s / kGX, d % kGX, d / kGX);
        TB_CHECK(latency_seen == want, "zero-load latency %d -> %d: %lld cycles, model %d", s, d,
                 (long long)latency_seen, want);
      }
  }

  // 2. Uniform random with random backpressure
  for (uint64_t i = 0; i < args.cycles; i++) {
    int rate = (i / 2000) % 2 ? 30 : 90, pop = (i / 3000) % 2 ? 40 : 95;
    RUN(cycle([&](int) { return (int)(rng() % 100) < rate ? (int)(rng() % kN) : -1; }, pop));
  }
  RUN(drain());

  // 3. Transpose and 4. hotspot
  if (kGX == kGY)
    for (uint64_t i = 0; i < args.cycles / 2; i++)
      RUN(cycle([&](int n) { return (rng() % 100) < 80 ? (n % kGX) * kGX + n / kGX : -1; }, 90));
  RUN(drain());
  int hot = (int)(rng() % kN);
  for (uint64_t i = 0; i < args.cycles / 2; i++)
    RUN(cycle([&](int) { return (rng() % 100) < 50 ? hot : -1; }, 100));
  RUN(drain());

  // 5. Neighbor permutation on X, then on Y: every ring link at 100%
  if (!expect_deadlock) {
    for (int dim = 0; dim < 2; dim++) {
      int ring = dim == 0 ? kGX : kGY;
      if (ring < 2) continue;
      auto nb = [&](int n) {
        int x = n % kGX, y = n / kGX;
        if (dim == 0) x = ring_next((uint8_t)x, kGX, kDir);
        else y = ring_next((uint8_t)y, kGY, kDir);
        return y * kGX + x;
      };
      for (int i = 0; i < 200; i++) RUN(cycle(nb, 100));
      std::fill(win_links.begin(), win_links.end(), 0);
      win_cycles = 0;
      win_on = true;
      for (int i = 0; i < 2000; i++) RUN(cycle(nb, 100));
      win_on = false;
      for (int n = 0; n < kN; n++) {
        double r = (double)win_links[n * kPorts + (dim == 0 ? kX : kY)] / win_cycles;
        TB_CHECK(r > 0.999, "neighbor permutation %s: node %d link at %.3f", dim ? "Y" : "X", n, r);
      }
      RUN(drain());
    }

    // 6. One stream across the diameter: 1 flit/cycle at the destination
    int far = (kDir == 0) ? (kGY - 1) * kGX + (kGX - 1) : 0;
    int src = (kDir == 0) ? 0 : kN - 1;
    auto stream = [&](int n) { return n == src ? far : -1; };
    for (int i = 0; i < 200; i++) RUN(cycle(stream, 100));
    win_ejected = 0;
    win_cycles = 0;
    win_on = true;
    for (int i = 0; i < 2000; i++) RUN(cycle(stream, 100));
    win_on = false;
    TB_CHECK((double)win_ejected / win_cycles > 0.999, "diameter stream at %.3f flits/cycle",
             (double)win_ejected / win_cycles);
    RUN(drain());
  }

  // 7. Saturating soak: every node always sends, ejection heavily backpressured
  for (uint64_t i = 0; i < soak; i++) {
    int pop = (i / 5000) % 2 ? 10 : 60;
    RUN(cycle([&](int) { return (int)(rng() % kN); }, pop));
  }
  RUN(drain());
  // Let the last pending sources go
  for (int i = 0; i < 2000; i++) RUN(cycle(none, 100));

  if (expect_deadlock) return finish(0);
  TB_CHECK(inflight.empty(), "%zu flits never delivered", inflight.size());
  for (int n = 0; n < kN; n++)
    for (int p = 0; p < kPorts; p++)
      TB_CHECK(rtl_links[n][p] == tlm.link_count(n, p), "link node %d port %s: rtl %llu flits, model %llu", n,
               kPortName[p], (unsigned long long)rtl_links[n][p], (unsigned long long)tlm.link_count(n, p));
  if (tfp) tfp->close();
  (void)deadlocked;
  printf("tb_noc: PASS %dx%d dir=%d slice=%d delivered=%llu cycles=%llu bubble_holds=%llu\n", kGX, kGY, kDir,
         kSlice, (unsigned long long)delivered, (unsigned long long)t, (unsigned long long)holds);
  return 0;
}
