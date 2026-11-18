// Bare NoC test (SPEC.md, Verification strategy: NoC; full-throughput rule).
//
// One plane (N_PLANES = 1, direction N_DIR) or both (N_PLANES = 2): endpoints
// at every node inject and eject, each flit on the plane select_plane() gives
// its source and destination, as every NIU does; the transaction-level model
// gives each flit's path. With N_IO_ROW, row GY-1 is the I/O row: only (0, GY-1)
// has an endpoint, the other positions are router-only stops inside the Y rings.
// Checks, in every phase:
//   - exactly-once delivery at the right node on the right plane, in order per
//     (source, destination);
//   - per-link flit counts equal the model's exactly at the end;
//   - zero bubbles; the deadlock watchdog never fires (no flit moves anywhere for
//     WATCHDOG cycles while flits are in flight -> dump and fail).
// Phases: zero-load latency for every source/destination pair (exact), uniform
// random with backpressure, transpose, hotspot, neighbor permutations along
// every ring direction (every link they use at 100%), a stream across the
// diameter (1 flit/cycle), and a saturating soak with heavy backpressure.
// --soak=N sets the soak length; --expect-deadlock inverts the result for the
// BUBBLE=0 build.
#include <verilated.h>
#include <verilated_fst_c.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <type_traits>
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
#ifndef N_PLANES
#define N_PLANES 1
#endif
#ifndef N_IO_ROW
#define N_IO_ROW 0
#endif

namespace {
constexpr int kGX = N_GX, kGY = N_GY, kN = kGX * kGY, kDir = N_DIR, kSlice = N_SLICE;
constexpr int kP = N_PLANES, kK = kP * kN;
constexpr bool kIoRow = N_IO_ROW;
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

// One bit of a Verilator port, scalar (up to 64 bits) or wide
template <typename T>
bool get_bit(const T& v, int bit) {
  if constexpr (std::is_integral_v<T>) return v >> bit & 1;
  else return v[bit / 32] >> (bit % 32) & 1;
}
template <typename T>
void set_bit(T& v, int bit, bool b) {
  if constexpr (std::is_integral_v<T>) {
    if (b) v |= (T)1 << bit;
    else v &= ~((T)1 << bit);
  } else {
    if (b) v[bit / 32] |= 1u << (bit % 32);
    else v[bit / 32] &= ~(1u << (bit % 32));
  }
}

// Nodes with an endpoint: all but the router-only stops of the I/O row
bool endpoint(int n) { return !(kIoRow && n / kGX == kGY - 1 && n % kGX != 0); }
// Plane index (0..kP-1) of a flow, and the ring direction of each plane index
int plane_of(int s, int d) { return kP == 2 ? select_plane(kGX, kGY, s % kGX, s / kGX, d % kGX, d / kGX) : 0; }
int dir_of(int p) { return kP == 2 ? p : kDir; }

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

  std::vector<int> eps;
  for (int n = 0; n < kN; n++) if (endpoint(n)) eps.push_back(n);
  auto random_ep = [&]() { return eps[rng() % eps.size()]; };

  std::vector<NocTlm> tlm;
  for (int p = 0; p < kP; p++) tlm.emplace_back(kGX, kGY, dir_of(p), kSlice);
  std::map<uint32_t, Inflight> inflight;
  std::map<std::pair<int, int>, uint32_t> last_id;
  std::vector<std::vector<uint64_t>> rtl_links(kK, std::vector<uint64_t>(kPorts, 0));
  // Per node: the next flit, then per plane the flit its sender offers
  std::vector<Flit> next(kN), pending(kK);
  std::vector<bool> have_next(kN, false), have(kK, false);
  uint32_t next_id = 1;
  uint64_t t = 0, idle = 0, delivered = 0, holds = 0;
  uint64_t per_plane[2] = {0, 0};

  // Per-cycle driver. dst_of(n) returns the destination node for a new flit at
  // endpoint n, or -1 for none; pop_p is the probability of popping a waiting flit.
  std::vector<uint64_t> win_links(kK * kPorts, 0);
  bool win_on = false;
  uint64_t win_cycles = 0, win_ejected = 0;
  int latency_probe_id = -1;
  int64_t latency_seen = -1;

  auto cycle = [&](auto dst_of, int pop_pct) -> int {
    for (int n : eps) {
      if (!have_next[n]) {
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
          next[n] = f;
          have_next[n] = true;
        }
      }
      if (have_next[n]) {
        int k = plane_of(n, next[n].dst_y * kGX + next[n].dst_x) * kN + n;
        if (!have[k]) {
          pending[k] = next[n];
          have[k] = true;
          have_next[n] = false;
        }
      }
    }
    for (int k = 0; k < kK; k++) {
      set_bit(dut->src_valid_i, k, have[k]);
      put_flit(dut->src_flit_i, k, pending[k]);
      set_bit(dut->pop_i, k, (int)(rng() % 100) < pop_pct);
    }
    half(0);

    bool progress = false;
    // Router events
    for (int k = 0; k < kK; k++)
      for (int p = 0; p < kPorts; p++) {
        int bit = k * kPorts + p;
        if (get_bit(dut->ev_flit_o, bit)) {
          rtl_links[k][p]++;
          progress = true;
          if (win_on) win_links[bit]++;
        }
        TB_CHECK(!get_bit(dut->ev_bubble_o, bit), "t=%llu bubble at plane %d node %d port %s",
                 (unsigned long long)t, k / kN, k % kN, kPortName[p]);
        holds += get_bit(dut->ev_bubble_hold_o, bit);
      }
    // Deliveries
    for (int k = 0; k < kK; k++) {
      if (!get_bit(dut->pop_i, k) || !get_bit(dut->rx_valid_o, k)) continue;
      int n = k % kN, pl = k / kN;
      Flit f = get_flit(dut->rx_head_o, k);
      uint32_t id = f.data[0];
      auto it = inflight.find(id);
      TB_CHECK(it != inflight.end(), "t=%llu node %d: flit %u delivered twice or never sent", (unsigned long long)t,
               n, id);
      TB_CHECK(it->second.dst == n, "t=%llu flit %u for node %d ejected at node %d", (unsigned long long)t, id,
               it->second.dst, n);
      TB_CHECK(plane_of(it->second.src, n) == pl, "t=%llu flit %u %d->%d ejected on plane index %d",
               (unsigned long long)t, id, it->second.src, n, pl);
      auto key = std::make_pair(it->second.src, n);
      TB_CHECK(id > last_id[key], "t=%llu flit %u out of order on %d->%d", (unsigned long long)t, id,
               it->second.src, n);
      last_id[key] = id;
      if ((int)id == latency_probe_id) latency_seen = (int64_t)(t - it->second.sent_at);
      tlm[pl].deliver(it->second.src % kGX, it->second.src / kGX, n % kGX, n / kGX);
      per_plane[pl]++;
      inflight.erase(it);
      delivered++;
      progress = true;
      if (win_on) win_ejected++;
    }
    // Sends
    for (int k = 0; k < kK; k++)
      if (get_bit(dut->src_taken_o, k)) {
        TB_CHECK(endpoint(k % kN), "t=%llu send from router-only stop %d", (unsigned long long)t, k % kN);
        inflight[pending[k].data[0]] = {k % kN, pending[k].dst_y * kGX + pending[k].dst_x, t};
        have[k] = false;
        progress = true;
      }
    half(1);
    t++;
    if (win_on) win_cycles++;
    idle = (progress || inflight.empty()) ? 0 : idle + 1;
    if (idle > kWatchdog) {
      fprintf(stderr, "WATCHDOG: no flit moved for %llu cycles with %zu in flight (t=%llu)\n",
              (unsigned long long)kWatchdog, inflight.size(), (unsigned long long)t);
      std::map<int, int> per_dst;
      for (auto& [id, f] : inflight) per_dst[f.dst]++;
      for (auto& [d, c] : per_dst) fprintf(stderr, "  %d flits in flight to node %d\n", c, d);
      for (int k = 0; k < kK; k++)
        for (int p = 0; p < kPorts; p++)
          if (get_bit(dut->ev_credit_stall_o, k * kPorts + p))
            fprintf(stderr, "  plane %d node %d (%d,%d) output %s: waiting, no credit\n", dir_of(k / kN), k % kN,
                    k % kN % kGX, k % kN / kGX, kPortName[p]);
      return 2;
    }
    return ctx->gotError() ? 1 : 0;
  };

  auto none = [](int) { return -1; };
  auto sources_pending = [&]() {
    for (int n = 0; n < kN; n++) if (have_next[n]) return true;
    for (int k = 0; k < kK; k++) if (have[k]) return true;
    return false;
  };
  auto drain = [&]() -> int {
    for (int i = 0; i < 2000 && !inflight.empty(); i++) {
      int r = cycle(none, 100);
      if (r) return r;
    }
    if (sources_pending()) return 0;  // pending sources still drain next phase
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

  // Stream mode (--streams=sx,sy,n --report=f), for the performance model's
  // cycle-exact check (model/noc_sim.py check-rtl): every compute endpoint
  // streams n flits to the one sx, sy away on the compute torus (the I/O row
  // skipped), all from the same cycle, ejection never stalls. The report lists
  // every flit: source, destination, plane index, cycle sent, cycle ejected.
  if (args.flag("streams")) {
    int sx = 0, sy = 0, n = 0;
    TB_CHECK(sscanf(args.opt("streams").c_str(), "%d,%d,%d", &sx, &sy, &n) == 3, "--streams=sx,sy,n");
    const int rows = kIoRow ? kGY - 1 : kGY;
    std::vector<int> left(kN, 0);
    auto partner = [&](int s) {
      int x = ((s % kGX + sx) % kGX + kGX) % kGX, y = ((s / kGX + sy) % rows + rows) % rows;
      return y * kGX + x;
    };
    for (int s : eps)
      if (s / kGX < rows) left[s] = n;
    std::map<uint32_t, std::array<uint64_t, 5>> log;   // id -> src, dst, plane, sent, ejected
    uint32_t first_id = next_id;
    uint64_t t0 = t;
    auto src = [&](int s) {
      if (!left[s]) return -1;
      left[s]--;
      return partner(s);
    };
    // Sent and ejected cycles come from the inflight table around each cycle
    auto run = [&](auto dst_of) -> int {
      std::map<uint32_t, Inflight> before = inflight;
      int r = cycle(dst_of, 100);
      for (auto& [id, f] : inflight)
        if (!before.count(id)) log[id] = {(uint64_t)f.src, (uint64_t)f.dst, (uint64_t)plane_of(f.src, f.dst),
                                          t - 1 - t0, 0};
      for (auto& [id, f] : before)
        if (!inflight.count(id)) log[id][4] = t - 1 - t0;
      return r;
    };
    while (sources_pending() || !inflight.empty() || std::any_of(left.begin(), left.end(), [](int v) { return v; }))
      RUN(run(src));
    FILE* f = fopen(args.opt("report").c_str(), "w");
    TB_CHECK(f, "cannot write --report");
    fprintf(f, "{\"gx\": %d, \"gy\": %d, \"planes\": %d, \"dir\": %d, \"io_row\": %d, \"slice\": %d, \"shift\": [%d, %d], "
            "\"n\": %d,\n \"flits\": [", kGX, kGY, kP, kDir, (int)kIoRow, kSlice, sx, sy, n);
    for (auto& [id, e] : log)
      fprintf(f, "%s\n  [%u, %llu, %llu, %llu, %llu, %llu]", id == first_id ? "" : ",", id - first_id,
              (unsigned long long)e[0], (unsigned long long)e[1], (unsigned long long)e[2], (unsigned long long)e[3],
              (unsigned long long)e[4]);
    fprintf(f, "]}\n");
    fclose(f);
    printf("tb_noc: PASS streams (%d,%d) x %d flits, %zu flits, %llu cycles\n", sx, sy, n, log.size(),
           (unsigned long long)(t - t0));
    return 0;
  }

  // 1. Zero-load latency, every source/destination pair, exact
  if (!expect_deadlock) {
    for (int s : eps)
      for (int d : eps) {
        bool sent = false;
        latency_probe_id = (int)next_id;
        latency_seen = -1;
        auto one = [&](int n) { if (n == s && !sent) { sent = true; return d; } return -1; };
        RUN(cycle(one, 100));
        for (int i = 0; i < 200 && latency_seen < 0; i++) RUN(cycle(none, 100));
        int want = tlm[plane_of(s, d)].zero_load_latency(s % kGX, s / kGX, d % kGX, d / kGX);
        TB_CHECK(latency_seen == want, "zero-load latency %d -> %d: %lld cycles, model %d", s, d,
                 (long long)latency_seen, want);
      }
  }

  // 2. Uniform random with random backpressure
  for (uint64_t i = 0; i < args.cycles; i++) {
    int rate = (i / 2000) % 2 ? 30 : 90, pop = (i / 3000) % 2 ? 40 : 95;
    RUN(cycle([&](int) { return (int)(rng() % 100) < rate ? random_ep() : -1; }, pop));
  }
  RUN(drain());

  // 3. Transpose and 4. hotspot
  if (kGX == kGY && !kIoRow)
    for (uint64_t i = 0; i < args.cycles / 2; i++)
      RUN(cycle([&](int n) { return (rng() % 100) < 80 ? (n % kGX) * kGX + n / kGX : -1; }, 90));
  RUN(drain());
  int hot = random_ep();
  for (uint64_t i = 0; i < args.cycles / 2; i++)
    RUN(cycle([&](int) { return (rng() % 100) < 50 ? hot : -1; }, 100));
  RUN(drain());

  // 5. Neighbor permutations along every ring direction the planes have:
  // every endpoint row (or column) streams to its next one; with the I/O row
  // the compute rows wrap over it. Every link a single flow uses runs at 100%.
  if (!expect_deadlock) {
    const int rows = kIoRow ? kGY - 1 : kGY;
    for (int p = 0; p < kP; p++)
      for (int dim = 0; dim < 2; dim++) {
        int dir = dir_of(p);
        if ((dim == 0 ? kGX : rows) < 2) continue;
        auto nb = [&](int n) {
          int x = n % kGX, y = n / kGX;
          if (y >= rows) return -1;   // the I/O tile stays idle
          if (dim == 0) x = ring_next((uint8_t)x, kGX, dir);
          else y = ring_next((uint8_t)y, rows, dir);
          return y * kGX + x;
        };
        // Flows per link, from the model's paths
        std::map<int, int> flows;
        for (int n : eps) {
          int d = nb(n);
          if (d < 0) continue;
          int pl = plane_of(n, d);
          for (auto [node, port] : tlm[pl].path(n % kGX, n / kGX, d % kGX, d / kGX))
            flows[(pl * kN + node) * kPorts + port]++;
        }
        for (int i = 0; i < 200; i++) RUN(cycle(nb, 100));
        std::fill(win_links.begin(), win_links.end(), 0);
        win_cycles = 0;
        win_on = true;
        for (int i = 0; i < 2000; i++) RUN(cycle(nb, 100));
        win_on = false;
        for (auto [bit, f] : flows) {
          double r = (double)win_links[bit] / win_cycles;
          int k = bit / kPorts;
          TB_CHECK(f == 1 && r > 0.999, "neighbor permutation dir %d %s: plane %d node %d port %s carries %d flows "
                   "at %.3f", dir, dim ? "Y" : "X", dir_of(k / kN), k % kN, kPortName[bit % kPorts], f, r);
        }
        RUN(drain());
      }

    // 6. One stream across the diameter: 1 flit/cycle at the destination
    int src = eps[0], far = eps[0], best = -1;
    for (int s : eps)
      for (int d : eps) {
        int h = tlm[plane_of(s, d)].hops(s % kGX, s / kGX, d % kGX, d / kGX);
        if (h > best) { best = h; src = s; far = d; }
      }
    auto stream = [&](int n) { return n == src ? far : -1; };
    for (int i = 0; i < 200; i++) RUN(cycle(stream, 100));
    win_ejected = 0;
    win_cycles = 0;
    win_on = true;
    for (int i = 0; i < 2000; i++) RUN(cycle(stream, 100));
    win_on = false;
    TB_CHECK((double)win_ejected / win_cycles > 0.999, "diameter stream %d -> %d (%d hops) at %.3f flits/cycle", src,
             far, best, (double)win_ejected / win_cycles);
    RUN(drain());
  }

  // 7. Saturating soak: every endpoint always sends, ejection heavily backpressured
  for (uint64_t i = 0; i < soak; i++) {
    int pop = (i / 5000) % 2 ? 10 : 60;
    RUN(cycle([&](int) { return random_ep(); }, pop));
  }
  RUN(drain());
  // Let the last pending sources go
  for (int i = 0; i < 2000; i++) RUN(cycle(none, 100));

  if (expect_deadlock) return finish(0);
  TB_CHECK(inflight.empty() && !sources_pending(), "%zu flits never delivered", inflight.size());
  for (int k = 0; k < kK; k++)
    for (int p = 0; p < kPorts; p++)
      TB_CHECK(rtl_links[k][p] == tlm[k / kN].link_count(k % kN, p),
               "link plane %d node %d port %s: rtl %llu flits, model %llu", dir_of(k / kN), k % kN, kPortName[p],
               (unsigned long long)rtl_links[k][p], (unsigned long long)tlm[k / kN].link_count(k % kN, p));
  if (tfp) tfp->close();
  printf("tb_noc: PASS %dx%d planes=%d%s dir=%d slice=%d delivered=%llu (NoC%d %llu, NoC1 %llu) cycles=%llu "
         "bubble_holds=%llu\n",
         kGX, kGY, kP, kIoRow ? " io_row" : "", kDir, kSlice, (unsigned long long)delivered, kP == 2 ? 0 : kDir,
         (unsigned long long)per_plane[0], (unsigned long long)per_plane[1], (unsigned long long)t,
         (unsigned long long)holds);
  return 0;
}
