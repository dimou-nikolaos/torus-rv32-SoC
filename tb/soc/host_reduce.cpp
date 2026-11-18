// All-reduce on the torus, loaded and launched from C++ through the I/O tile
// (SPEC.md, M4). Every tile gets a random int32 vector; sw/kernels/reduce
// leaves the element-wise sum (modulo 2^32) on every tile, checked bit-exact,
// and the report carries the timings and counters for the performance model.
//
//   host_reduce <reduce.elf> [--len=BYTES] [--plane=auto|noc0|noc1] [--report=FILE]
//               [SimHost options: --seed, --cycles, --trace, --no-cosim, ...]
#include <algorithm>
#include <array>
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

// The vector, the receive buffers (up to 8 levels of 2 KB, below the loader at 0xE000), timings
constexpr uint32_t kVec = 0x8000, kRecv = 0x9000, kResult = 0xF100;
constexpr uint32_t kMaxLen = 2048;
const std::vector<unsigned> kModelSelects = {kEvNiuTx0, kEvNiuTx1, kEvNiuRx0, kEvNiuRx1,
                                             kEvR0Flit + kPortX, kEvR0Flit + kPortY, kEvR1Flit + kPortX,
                                             kEvR1Flit + kPortY};

struct Args {
  uint32_t vec, recv, stride, len, plane, result;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 50'000'000);
  if (args.positional.size() != 1) {
    fprintf(stderr, "usage: host_reduce <reduce.elf> [--len=BYTES] [--plane=auto|noc0|noc1] [--report=FILE]\n");
    return 2;
  }
  const uint32_t len = (uint32_t)std::stoul(args.opt("len", "1024"));
  const std::string plane_name = args.opt("plane", "auto");
  const uint32_t plane = plane_name == "noc0" ? 1 : plane_name == "noc1" ? 2 : 0;
  if (plane == 0 && plane_name != "auto") {
    fprintf(stderr, "--plane must be auto, noc0 or noc1\n");
    return 2;
  }
  if (len == 0 || len % 16 || len > kMaxLen) {
    fprintf(stderr, "--len must be a multiple of 16 up to %u\n", kMaxLen);
    return 2;
  }
  std::mt19937_64 rng(args.seed);
  SimHost sim(argc, argv, args);
  int status = 0;
  try {
    Device dev(sim);
    auto grid = dev.compute_grid();
    const unsigned px = dev.nx(), py = dev.ny(), tiles = (unsigned)grid.size(), n = len / 4;
    auto index = [&](Xy t) { return t.y * px + t.x; };

    std::vector<std::vector<uint32_t>> vecs(tiles, std::vector<uint32_t>(n));
    std::vector<uint32_t> sum(n, 0);
    for (auto& v : vecs)
      for (unsigned i = 0; i < n; i++) {
        v[i] = (uint32_t)rng();
        sum[i] += v[i];
      }

    // Load time: every image written and landed (--load=host or tree)
    uint64_t load_cycles = 0;
    Program prog = host_load(dev, sim, args, args.positional[0], grid, &load_cycles);
    std::vector<unsigned> selects = kModelSelects;
    dev.select_counters(grid, selects);
    if (!args.flag("no-cosim"))
      for (unsigned i = 0; i < tiles; i++) sim.attach_cosim((int)i, prog.images[i]);
    for (Xy t : grid) {
      dev.write(t, kVec, vecs[index(t)].data(), len);
      Args ar{kVec, kRecv, len, len, plane, kResult};
      dev.set_args(prog, t, &ar, sizeof ar);
    }
    uint64_t t0 = sim.cycles();
    dev.launch(prog);
    while (dev.done_count() < tiles && !sim.failed()) sim.run(1000);
    uint64_t run = sim.cycles() - t0;
    sim.run(300);
    dev.stop(prog);
    if (sim.failed()) throw std::runtime_error("RTL assertion or lockstep mismatch");

    // Results: the sum on every tile
    auto codes = dev.exit_codes(prog);
    std::vector<uint32_t> got(n);
    for (Xy t : grid) {
      unsigned i = index(t);
      if (codes[i] != 0) {
        fprintf(stderr, "tile (%u,%u) exited with %d\n", t.x, t.y, codes[i]);
        status = 1;
      }
      dev.read(t, kVec, got.data(), len);
      for (unsigned k = 0; k < n; k++)
        if (got[k] != sum[k]) {
          fprintf(stderr, "tile (%u,%u): element %u is %08x, expected %08x\n", t.x, t.y, k, got[k], sum[k]);
          status = 1;
          break;
        }
    }
    auto counters = dev.read_counters(grid);
    // Per tile: start, cycles in the add loops, end
    std::vector<std::array<uint32_t, 4>> times(tiles);
    uint32_t first = ~0u, last = 0, add_max = 0;
    for (Xy t : grid) {
      auto& tm = times[index(t)];
      dev.read(t, kResult, tm.data(), 16);
      first = std::min(first, tm[0]);
      last = std::max(last, tm[2]);
      add_max = std::max(add_max, tm[1]);
    }
    printf("reduce %ux%u, %u bytes per tile, plane %s: kernel %u cycles, add loops up to %u cycles on one tile, "
           "run %llu cycles, program load %llu cycles\n",
           px, py, len, plane_name.c_str(), last - first, add_max, (unsigned long long)run,
           (unsigned long long)load_cycles);
    for (unsigned i = 0; i < tiles; i++)
      if (sim.tile((int)i).cosim && sim.tile((int)i).cosim_failed)
        printf("lockstep tile %u,%u: MISMATCH\n", i % px, i / px);

    if (args.flag("report")) {
      FILE* f = fopen(args.opt("report").c_str(), "w");
      if (!f) throw std::runtime_error("cannot write " + args.opt("report"));
      fprintf(f, "{\n  \"kernel\": \"reduce\", \"nx\": %u, \"ny\": %u, \"len\": %u, \"plane\": \"%s\",\n", px, py, len,
              plane_name.c_str());
      fprintf(f, "  \"kernel_cycles\": %u, \"run_cycles\": %llu, \"load_cycles\": %llu, \"load\": \"%s\", \"seed\": %llu,\n"
              "  \"events\": [",
              last - first, (unsigned long long)run, (unsigned long long)load_cycles,
              args.opt("load", "host").c_str(), (unsigned long long)args.seed);
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
  printf("host_reduce: %s\n", status == 0 ? "PASS" : "FAIL");
  return status;
}
