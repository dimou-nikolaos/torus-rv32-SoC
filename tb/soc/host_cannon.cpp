// Cannon GEMM on the torus, loaded and launched from C++ through the I/O tile
// (SPEC.md, M3 exit test). C = A B for N x N int32 matrices (modulo 2^32) on
// a P x P compute grid, one b x b block per tile (b = N / P, at most 32). The
// host skews the blocks as it loads them (tile (x, y) gets A(y, (x + y) mod P)
// and B((x + y) mod P, x)), runs sw/kernels/cannon, checks every C block
// bit-exact against a C++ reference, and writes a report with the per-step
// timings and counters for the performance model (model/perf_model.py).
//
//   host_cannon <cannon.elf> [--n=N] [--plane=auto|noc0|noc1] [--report=FILE]
//               [--events=E0,..,E7] [SimHost options: --seed, --cycles, --trace, --no-cosim, ...]
// --plane forces all of the kernel's traffic onto one plane (a one-plane NoC
// for comparisons); --events selects other counter events than the ones the
// model checks and prints them per tile.
#include <algorithm>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "host_load.h"
#include "sim_host.h"
#include "tb_args.h"
#include "torus.h"

using namespace torus;

namespace {

// Block buffers (4 KB each: b <= 32), the same on every tile: above the kernel
// in the program region, or in the upper half of 64 KB scratchpads; and the
// timings
constexpr uint32_t kResult = kSync + 0x100;
constexpr unsigned kMaxBlock = 32;
const std::vector<unsigned> kModelSelects = {kEvNiuTx0, kEvNiuTx1, kEvNiuRx0, kEvNiuRx1,
                                             kEvR0Flit + kPortX, kEvR0Flit + kPortY, kEvR1Flit + kPortX,
                                             kEvR1Flit + kPortY};

struct Args {
  uint32_t a[2], b[2], c, bs, plane, result;
};

std::vector<unsigned> parse_list(const std::string& s) {
  std::vector<unsigned> v;
  for (size_t pos = 0; pos < s.size();) {
    size_t c = s.find(',', pos);
    v.push_back((unsigned)std::stoul(s.substr(pos, c - pos)));
    pos = c == std::string::npos ? s.size() : c + 1;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 50'000'000);
  if (args.positional.size() != 1) {
    fprintf(stderr, "usage: host_cannon <cannon.elf> [--n=N] [--plane=auto|noc0|noc1] [--report=FILE]\n");
    return 2;
  }
  const unsigned n = (unsigned)std::stoul(args.opt("n", "64"));
  const std::string plane_name = args.opt("plane", "auto");
  const uint32_t plane = plane_name == "noc0" ? 1 : plane_name == "noc1" ? 2 : 0;
  if (plane == 0 && plane_name != "auto") {
    fprintf(stderr, "--plane must be auto, noc0 or noc1\n");
    return 2;
  }
  std::mt19937_64 rng(args.seed);
  SimHost sim(argc, argv, args);
  int status = 0;
  try {
    Device dev(sim);
    auto grid = dev.compute_grid();
    const unsigned p = dev.nx(), tiles = (unsigned)grid.size();
    if (dev.ny() != p) throw std::runtime_error("Cannon needs a square compute grid");
    if (n % p || n / p > kMaxBlock || n == 0) throw std::runtime_error("--n must be a multiple of P, at most 32 P");
    const unsigned b = n / p;
    const uint32_t base = dev.spm_bytes() >= 64 * 1024 ? 0x8000 : 0x1800;
    const uint32_t kA0 = base, kA1 = base + 0x1000, kB0 = base + 0x2000, kB1 = base + 0x3000, kC = base + 0x4000;
    auto index = [&](Xy t) { return t.y * p + t.x; };

    // Matrices and the reference product, modulo 2^32
    std::vector<uint32_t> A(n * n), B(n * n), C(n * n, 0);
    for (auto& v : A) v = (uint32_t)rng();
    for (auto& v : B) v = (uint32_t)rng();
    for (unsigned i = 0; i < n; i++)
      for (unsigned k = 0; k < n; k++) {
        uint32_t aik = A[i * n + k];
        for (unsigned j = 0; j < n; j++) C[i * n + j] += aik * B[k * n + j];
      }
    auto block = [&](const std::vector<uint32_t>& m, unsigned bi, unsigned bj) {
      std::vector<uint32_t> blk(b * b);
      for (unsigned r = 0; r < b; r++)
        for (unsigned c = 0; c < b; c++) blk[r * b + c] = m[(bi * b + r) * n + bj * b + c];
      return blk;
    };

    // Load time: every image written and landed (--load=host or tree)
    uint64_t load_cycles = 0;
    Program prog = host_load(dev, sim, args, args.positional[0], grid, &load_cycles);
    std::vector<unsigned> selects = args.flag("events") ? parse_list(args.opt("events")) : kModelSelects;
    dev.select_counters(grid, selects);
    if (!args.flag("no-cosim"))
      for (unsigned i = 0; i < tiles; i++) sim.attach_cosim((int)i, prog.images[i]);
    const uint32_t bytes = b * b * 4;
    std::vector<uint32_t> zero(b * b, 0), stale(b * b, 0xA5A5A5A5u);
    for (Xy t : grid) {
      unsigned i = t.y, j = t.x, s = (i + j) % p;
      auto a = block(A, i, s), bb = block(B, s, j);
      dev.write(t, kA0, a.data(), bytes);
      dev.write(t, kB0, bb.data(), bytes);
      dev.write(t, kA1, stale.data(), bytes);   // stale data must not pass for a shifted block
      dev.write(t, kB1, stale.data(), bytes);
      dev.write(t, kC, zero.data(), bytes);
      Args ar{{kA0, kA1}, {kB0, kB1}, kC, b, plane, kResult};
      dev.set_args(prog, t, &ar, sizeof ar);
    }
    uint64_t t0 = sim.cycles();
    dev.launch(prog);
    while (dev.done_count() < tiles && !sim.failed()) sim.run(1000);
    uint64_t run = sim.cycles() - t0;
    sim.run(300);
    dev.stop(prog);
    if (sim.failed()) throw std::runtime_error("RTL assertion or lockstep mismatch");

    // Results: every C block bit-exact
    auto codes = dev.exit_codes(prog);
    std::vector<uint32_t> got(b * b);
    for (Xy t : grid) {
      unsigned i = index(t);
      if (codes[i] != 0) {
        fprintf(stderr, "tile (%u,%u) exited with %d\n", t.x, t.y, codes[i]);
        status = 1;
      }
      dev.read(t, kC, got.data(), bytes);
      auto want = block(C, t.y, t.x);
      for (unsigned k = 0; k < b * b; k++)
        if (got[k] != want[k]) {
          fprintf(stderr, "tile (%u,%u): C(%u,%u) is %08x, expected %08x\n", t.x, t.y, t.y * b + k / b,
                  t.x * b + k % b, got[k], want[k]);
          status = 1;
          break;
        }
    }
    auto counters = dev.read_counters(grid);
    // Per step k: start, compute begins, compute done, ready (4k .. 4k+3)
    std::vector<std::vector<uint32_t>> times(tiles, std::vector<uint32_t>(4 * p));
    uint32_t first = ~0u, last = 0;
    for (Xy t : grid) {
      auto& tm = times[index(t)];
      dev.read(t, kResult, tm.data(), 4 * tm.size());
      first = std::min(first, tm[0]);
      last = std::max(last, tm[4 * p - 1]);
    }
    // Mean per step and tile: the block multiply, and everything else
    // (hand-over waits, DMA programming, semaphores)
    double compute = 0, sync = 0;
    for (auto& tm : times)
      for (unsigned k = 0; k < p; k++) {
        compute += tm[4 * k + 2] - tm[4 * k + 1];
        sync += (tm[4 * k + 3] - tm[4 * k]) - (tm[4 * k + 2] - tm[4 * k + 1]);
      }
    compute /= tiles * p;
    sync /= tiles * p;
    printf("cannon %ux%u, N=%u (b=%u), plane %s: kernel %u cycles, compute %.0f + sync %.0f cycles per step "
           "(mean), run %llu cycles, program load %llu cycles\n",
           p, p, n, b, plane_name.c_str(), last - first, compute, sync, (unsigned long long)run,
           (unsigned long long)load_cycles);
    if (args.flag("events"))
      for (Xy t : grid) {
        printf("tile %u,%u events", t.x, t.y);
        for (size_t k = 0; k < selects.size(); k++)
          printf(" %u:%llu", selects[k], (unsigned long long)counters[index(t)][2 + k]);
        printf("\n");
      }
    for (unsigned i = 0; i < tiles; i++)
      if (sim.tile((int)i).cosim)
        printf("lockstep tile %u,%u: %llu instructions, %s\n", i % p, i / p,
               (unsigned long long)sim.tile((int)i).cosim_retired,
               sim.tile((int)i).cosim_failed ? "MISMATCH" : "0 mismatches");

    if (args.flag("report")) {
      FILE* f = fopen(args.opt("report").c_str(), "w");
      if (!f) throw std::runtime_error("cannot write " + args.opt("report"));
      fprintf(f, "{\n  \"kernel\": \"cannon\", \"nx\": %u, \"ny\": %u, \"n\": %u, \"b\": %u, \"plane\": \"%s\",\n", p,
              p, n, b, plane_name.c_str());
      fprintf(f, "  \"kernel_cycles\": %u, \"run_cycles\": %llu, \"load_cycles\": %llu, \"load\": \"%s\", \"seed\": %llu,\n"
              "  \"events\": [",
              last - first, (unsigned long long)run, (unsigned long long)load_cycles,
              args.opt("load", "host").c_str(), (unsigned long long)args.seed);
      for (size_t k = 0; k < selects.size(); k++) fprintf(f, "%s%u", k ? ", " : "", selects[k]);
      fprintf(f, "],\n  \"tiles\": [\n");
      for (Xy t : grid) {
        unsigned i = index(t);
        fprintf(f, "    {\"x\": %u, \"y\": %u, \"t\": [", t.x, t.y);
        for (unsigned k = 0; k < 4 * p; k++) fprintf(f, "%s%u", k ? ", " : "", times[i][k] - first);
        fprintf(f, "], \"counters\": [");
        for (size_t k = 0; k < counters[i].size(); k++)
          fprintf(f, "%s%llu", k ? ", " : "", (unsigned long long)counters[i][k]);
        fprintf(f, "]}%s\n", i + 1 < tiles ? "," : "");
      }
      fprintf(f, "  ]\n}\n");
      fclose(f);
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    status = 1;
  }
  sim.finish();
  printf("host_cannon: %s\n", status == 0 ? "PASS" : "FAIL");
  return status;
}
