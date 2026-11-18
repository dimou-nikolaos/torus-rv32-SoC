// memcpy on the torus, loaded and launched from C++ through the I/O tile
// (SPEC.md, M2 exit test). The host fills a source block in every compute
// tile, gives each tile its partner (a fixed shift on the compute grid), runs
// sw/kernels/memcpy, checks every destination block bit-exact, and writes a
// report with the per-tile timings and counters for the performance model
// (model/perf_model.py).
//
//   host_memcpy <memcpy.elf> [--len=BYTES] [--shift=DX,DY] [--report=FILE]
//               [--events=E0,..,E7] [SimHost options: --seed, --cycles, --trace, --no-cosim, ...]
// --events selects other counter events (tile_pkg.sv perf_event_e) than the
// ones the model checks, for investigations; they are printed per tile.
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

constexpr uint32_t kSrc = 0x4000, kDst = 0x8000, kSem = 0xF000, kResult = 0xF010;
// Counters 2..9 count these (the model maps them by event number)
const std::vector<unsigned> kModelSelects = {kEvNiuTx0, kEvNiuTx1, kEvNiuRx0, kEvNiuRx1,
                                        kEvR0Flit + kPortX, kEvR0Flit + kPortY, kEvR1Flit + kPortX,
                                        kEvR1Flit + kPortY};

struct Args {
  uint32_t src, dst, len, partner, senders, sem, result;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 5'000'000);
  if (args.positional.size() != 1) {
    fprintf(stderr, "usage: host_memcpy <memcpy.elf> [--len=N] [--shift=DX,DY] [--report=FILE]\n");
    return 2;
  }
  uint32_t len = (uint32_t)std::stoul(args.opt("len", "16384"));
  std::string shift = args.opt("shift", "1,1");
  int sx = std::stoi(shift.substr(0, shift.find(','))), sy = std::stoi(shift.substr(shift.find(',') + 1));
  if (len % 16 || len == 0 || len > kDst - kSrc) {
    fprintf(stderr, "--len must be a multiple of 16 up to %u\n", kDst - kSrc);
    return 2;
  }
  std::mt19937_64 rng(args.seed);
  SimHost sim(argc, argv, args);
  int status = 0;
  try {
    Device dev(sim);
    auto grid = dev.compute_grid();
    const unsigned nx = dev.nx(), ny = dev.ny(), n = (unsigned)grid.size();
    auto index = [&](Xy t) { return t.y * nx + t.x; };
    auto partner = [&](Xy t) {
      return Xy{(t.x + nx + (unsigned)(sx % (int)nx)) % nx, (t.y + ny + (unsigned)(sy % (int)ny)) % ny};
    };
    std::vector<unsigned> senders(n, 0);
    for (Xy t : grid) senders[index(partner(t))]++;

    Program prog = dev.load_kernel(args.positional[0], grid);
    std::vector<unsigned> selects = kModelSelects;
    if (args.flag("events")) {
      selects.clear();
      std::string e = args.opt("events");
      for (size_t pos = 0; pos < e.size();) {
        size_t c = e.find(',', pos);
        selects.push_back((unsigned)std::stoul(e.substr(pos, c - pos)));
        pos = c == std::string::npos ? e.size() : c + 1;
      }
    }
    dev.select_counters(grid, selects);
    if (!args.flag("no-cosim"))
      for (unsigned i = 0; i < n; i++) sim.attach_cosim((int)i, prog.images[i]);
    std::vector<std::vector<uint8_t>> src(n, std::vector<uint8_t>(len));
    for (Xy t : grid) {
      auto& s = src[index(t)];
      for (auto& b : s) b = (uint8_t)rng();
      dev.write(t, kSrc, s.data(), len);
      std::vector<uint8_t> fill(len, 0xA5);   // stale data must not pass for a copy
      dev.write(t, kDst, fill.data(), len);
      uint32_t zero[4] = {0, 0, 0, 0};
      dev.write(t, kSem, zero, 16);
      dev.write(t, kResult, zero, 16);
      Xy p = partner(t);
      Args a{kSrc, kDst, len, p.x | p.y << 8, senders[index(t)], kSem, kResult};
      dev.set_args(prog, t, &a, sizeof a);
    }
    uint64_t t0 = sim.cycles();
    dev.launch(prog);
    while (dev.done_count() < n && !sim.failed()) sim.run(100);
    uint64_t run = sim.cycles() - t0;
    sim.run(300);
    dev.stop(prog);
    if (sim.failed()) throw std::runtime_error("RTL assertion or lockstep mismatch");

    // Results: every destination holds its sender's block
    auto codes = dev.exit_codes(prog);
    std::vector<uint8_t> got(len);
    for (Xy t : grid) {
      unsigned i = index(t);
      if (codes[i] != 0) {
        fprintf(stderr, "tile (%u,%u) exited with %d\n", t.x, t.y, codes[i]);
        status = 1;
      }
      dev.read(partner(t), kDst, got.data(), len);
      if (got != src[i]) {
        size_t k = 0;
        while (got[k] == src[i][k]) k++;
        fprintf(stderr, "tile (%u,%u) -> (%u,%u): byte %zu is 0x%02x, expected 0x%02x\n", t.x, t.y, partner(t).x,
                partner(t).y, k, got[k], src[i][k]);
        status = 1;
      }
    }
    auto counters = dev.read_counters(grid);
    std::vector<std::array<uint32_t, 4>> times(n);
    uint32_t first = ~0u, last = 0;
    for (Xy t : grid) {
      auto& tm = times[index(t)];
      dev.read(t, kResult, tm.data(), 16);
      first = std::min(first, tm[0]);
      last = std::max(last, tm[2]);
    }
    uint32_t copy = last - first;
    if (args.flag("events"))
      for (Xy t : grid) {
        printf("tile %u,%u t=[%u %u %u] events", t.x, t.y, times[index(t)][0] - first, times[index(t)][1] - first,
               times[index(t)][2] - first);
        for (size_t k = 0; k < selects.size(); k++)
          printf(" %u:%llu", selects[k], (unsigned long long)counters[index(t)][2 + k]);
        printf("\n");
      }
    printf("memcpy %ux%u, %u bytes per tile, shift (%d,%d): copy phase %u cycles (%.2f bytes/cycle per tile), "
           "run %llu cycles\n",
           nx, ny, len, sx, sy, copy, (double)len / copy, (unsigned long long)run);
    for (unsigned i = 0; i < n; i++)
      if (sim.tile((int)i).cosim)
        printf("lockstep tile %u,%u: %llu instructions, %s\n", i % nx, i / nx,
               (unsigned long long)sim.tile((int)i).cosim_retired,
               sim.tile((int)i).cosim_failed ? "MISMATCH" : "0 mismatches");

    if (args.flag("report")) {
      FILE* f = fopen(args.opt("report").c_str(), "w");
      if (!f) throw std::runtime_error("cannot write " + args.opt("report"));
      fprintf(f, "{\n  \"kernel\": \"memcpy\", \"nx\": %u, \"ny\": %u, \"len\": %u, \"shift\": [%d, %d],\n", nx, ny,
              len, sx, sy);
      fprintf(f, "  \"copy_cycles\": %u, \"run_cycles\": %llu, \"seed\": %llu,\n  \"events\": [", copy,
              (unsigned long long)run, (unsigned long long)args.seed);
      for (size_t k = 0; k < selects.size(); k++) fprintf(f, "%s%u", k ? ", " : "", selects[k]);
      fprintf(f, "],\n  \"tiles\": [\n");
      for (Xy t : grid) {
        unsigned i = index(t);
        Xy p = partner(t);
        fprintf(f, "    {\"x\": %u, \"y\": %u, \"partner\": [%u, %u], \"t\": [%u, %u, %u], \"counters\": [", t.x, t.y,
                p.x, p.y, times[i][0] - first, times[i][1] - first, times[i][2] - first);
        for (size_t k = 0; k < counters[i].size(); k++)
          fprintf(f, "%s%llu", k ? ", " : "", (unsigned long long)counters[i][k]);
        fprintf(f, "]}%s\n", i + 1 < n ? "," : "");
      }
      fprintf(f, "  ]\n}\n");
      fclose(f);
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    status = 1;
  }
  sim.finish();
  printf("host_memcpy: %s\n", status == 0 ? "PASS" : "FAIL");
  return status;
}
