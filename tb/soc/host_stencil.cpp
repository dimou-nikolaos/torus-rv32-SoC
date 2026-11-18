// 2D five-point stencil on the torus, loaded and launched from C++ through
// the I/O tile (SPEC.md, M4). A periodic G x G int32 grid (G = P b, b at most
// 32) is split into one b x b block per tile; the host runs sw/kernels/stencil
// for T steps, checks every block bit-exact against a C++ reference, and
// writes a report with the timings and counters for the performance model.
//
//   host_stencil <stencil.elf> [--b=B] [--steps=T] [--plane=auto|noc0|noc1] [--report=FILE]
//                [SimHost options: --seed, --cycles, --trace, --no-cosim, ...]
#include <algorithm>
#include <array>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "sim_host.h"
#include "tb_args.h"
#include "torus.h"

using namespace torus;

namespace {

// Block buffers (4 KB each), halos, packed columns, timings: the same on every tile
constexpr uint32_t kU0 = 0x8000, kU1 = 0x9000, kHalo = 0xA000, kSend = 0xA400, kResult = 0xF100;
constexpr unsigned kMaxBlock = 32;
const std::vector<unsigned> kModelSelects = {kEvNiuTx0, kEvNiuTx1, kEvNiuRx0, kEvNiuRx1,
                                             kEvR0Flit + kPortX, kEvR0Flit + kPortY, kEvR1Flit + kPortX,
                                             kEvR1Flit + kPortY};

struct Args {
  uint32_t u[2], halo, send, bs, steps, plane, result;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 50'000'000);
  if (args.positional.size() != 1) {
    fprintf(stderr, "usage: host_stencil <stencil.elf> [--b=B] [--steps=T] [--plane=auto|noc0|noc1] [--report=FILE]\n");
    return 2;
  }
  const unsigned b = (unsigned)std::stoul(args.opt("b", "16"));
  const unsigned steps = (unsigned)std::stoul(args.opt("steps", "8"));
  const std::string plane_name = args.opt("plane", "auto");
  const uint32_t plane = plane_name == "noc0" ? 1 : plane_name == "noc1" ? 2 : 0;
  if (plane == 0 && plane_name != "auto") {
    fprintf(stderr, "--plane must be auto, noc0 or noc1\n");
    return 2;
  }
  if (b < 4 || b > kMaxBlock || b % 4) {
    fprintf(stderr, "--b must be a multiple of 4 from 4 to %u\n", kMaxBlock);
    return 2;
  }
  std::mt19937_64 rng(args.seed);
  SimHost sim(argc, argv, args);
  int status = 0;
  try {
    Device dev(sim);
    auto grid = dev.compute_grid();
    const unsigned px = dev.nx(), py = dev.ny(), tiles = (unsigned)grid.size();
    const unsigned gw = px * b, gh = py * b;
    auto index = [&](Xy t) { return t.y * px + t.x; };

    // The grid and the reference: values below 2^20, so no sum overflows
    std::vector<int32_t> u(gw * gh), v(gw * gh);
    for (auto& e : u) e = (int32_t)(rng() & 0xFFFFF);
    std::vector<int32_t> u0 = u;
    for (unsigned t = 0; t < steps; t++) {
      for (unsigned i = 0; i < gh; i++)
        for (unsigned j = 0; j < gw; j++) {
          auto at = [&](unsigned r, unsigned c) { return u[(r % gh) * gw + c % gw]; };
          v[i * gw + j] = (at(i + gh - 1, j) + at(i + 1, j) + at(i, j + gw - 1) + at(i, j + 1) + 4 * at(i, j)) >> 3;
        }
      std::swap(u, v);
    }
    auto block = [&](const std::vector<int32_t>& m, Xy t) {
      std::vector<int32_t> blk(b * b);
      for (unsigned r = 0; r < b; r++)
        for (unsigned c = 0; c < b; c++) blk[r * b + c] = m[(t.y * b + r) * gw + t.x * b + c];
      return blk;
    };

    // Load time: every image written and landed (the writes are posted)
    uint64_t t_load = sim.cycles();
    Program prog = dev.load_kernel(args.positional[0], grid);
    dev.sync();
    const uint64_t load_cycles = sim.cycles() - t_load;
    std::vector<unsigned> selects = kModelSelects;
    dev.select_counters(grid, selects);
    if (!args.flag("no-cosim"))
      for (unsigned i = 0; i < tiles; i++) sim.attach_cosim((int)i, prog.images[i]);
    const uint32_t bytes = b * b * 4;
    for (Xy t : grid) {
      auto blk = block(u0, t);
      dev.write(t, kU0, blk.data(), bytes);
      Args ar{{kU0, kU1}, kHalo, kSend, b, steps, plane, kResult};
      dev.set_args(prog, t, &ar, sizeof ar);
    }
    uint64_t t0 = sim.cycles();
    dev.launch(prog);
    while (dev.done_count() < tiles && !sim.failed()) sim.run(1000);
    uint64_t run = sim.cycles() - t0;
    sim.run(300);
    dev.stop(prog);
    if (sim.failed()) throw std::runtime_error("RTL assertion or lockstep mismatch");

    // Results: every block bit-exact after T steps
    auto codes = dev.exit_codes(prog);
    std::vector<int32_t> got(b * b);
    for (Xy t : grid) {
      unsigned i = index(t);
      if (codes[i] != 0) {
        fprintf(stderr, "tile (%u,%u) exited with %d\n", t.x, t.y, codes[i]);
        status = 1;
      }
      dev.read(t, steps % 2 ? kU1 : kU0, got.data(), bytes);
      auto want = block(u, t);
      for (unsigned k = 0; k < b * b; k++)
        if (got[k] != want[k]) {
          fprintf(stderr, "tile (%u,%u): u(%u,%u) is %d, expected %d\n", t.x, t.y, t.y * b + k / b, t.x * b + k % b,
                  got[k], want[k]);
          status = 1;
          break;
        }
    }
    auto counters = dev.read_counters(grid);
    // Per tile: start, cycles in the compute loops, end
    std::vector<std::array<uint32_t, 4>> times(tiles);
    uint32_t first = ~0u, last = 0;
    double compute = 0;
    for (Xy t : grid) {
      auto& tm = times[index(t)];
      dev.read(t, kResult, tm.data(), 16);
      first = std::min(first, tm[0]);
      last = std::max(last, tm[2]);
      compute += tm[1];
    }
    compute /= (double)tiles * steps;
    printf("stencil %ux%u, %ux%u grid (b=%u), %u steps, plane %s: kernel %u cycles, %.0f per step, compute %.0f "
           "per step (mean), run %llu cycles, program load %llu cycles\n",
           px, py, gw, gh, b, steps, plane_name.c_str(), last - first, (double)(last - first) / steps, compute,
           (unsigned long long)run, (unsigned long long)load_cycles);
    for (unsigned i = 0; i < tiles; i++)
      if (sim.tile((int)i).cosim && sim.tile((int)i).cosim_failed)
        printf("lockstep tile %u,%u: MISMATCH\n", i % px, i / px);

    if (args.flag("report")) {
      FILE* f = fopen(args.opt("report").c_str(), "w");
      if (!f) throw std::runtime_error("cannot write " + args.opt("report"));
      fprintf(f, "{\n  \"kernel\": \"stencil\", \"nx\": %u, \"ny\": %u, \"b\": %u, \"steps\": %u, \"plane\": \"%s\",\n",
              px, py, b, steps, plane_name.c_str());
      fprintf(f, "  \"kernel_cycles\": %u, \"run_cycles\": %llu, \"load_cycles\": %llu, \"seed\": %llu,\n"
              "  \"events\": [",
              last - first, (unsigned long long)run, (unsigned long long)load_cycles,
              (unsigned long long)args.seed);
      for (size_t k = 0; k < selects.size(); k++) fprintf(f, "%s%u", k ? ", " : "", selects[k]);
      fprintf(f, "],\n  \"tiles\": [\n");
      for (Xy t : grid) {
        unsigned i = index(t);
        fprintf(f, "    {\"x\": %u, \"y\": %u, \"t\": [%u, %u, %u], \"counters\": [", t.x, t.y, times[i][0] - first,
                times[i][1], times[i][2] - first);
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
  printf("host_stencil: %s\n", status == 0 ? "PASS" : "FAIL");
  return status;
}
