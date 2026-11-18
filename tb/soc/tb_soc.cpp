// SoC testbench: runs programs on an NX x NY torus through the I/O tile, with
// libtorus as the host (SPEC.md, launch protocol).
//
//   tb_soc <program.elf> [more.elf ...] [--seed=N] [--cycles=MAX] [--trace=f.fst]
//          [--no-cosim] [--no-verify] [--isa=...] [--rvfi-trace]
//          [--axi-half=N] [--fabric-half=N]
//
// One ELF runs on every tile (kernels use my_xy()), or one ELF per tile in
// tile order (n = y * NX + x). Flow: find the I/O tile, check every tile's
// registers over the NoC and that bad addresses answer SLVERR; load the images
// through the I/O tile and read them back; launch; poll DONE until every tile
// has exited, with Spike in lockstep on every tile; stop the cores, read every
// counter bank, and check the counters per tile and across the grid (every
// flit injected was retired, no NIU errors) and the exit words against the
// console. Exit status: nonzero on any mismatch or timeout, else the first
// nonzero program exit code.
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "sim_host.h"
#include "tb_args.h"
#include "torus.h"

using namespace torus;

namespace {

// Counter indices, with the selects the counter bank has after reset (the
// programs may rely on them, so the testbench keeps them)
enum { C_CYCLES = 0, C_RETIRED = 1, C_FETCH = 2, C_DATA = 3, C_NIU_TX0 = 4, C_NIU_TX1 = 5, C_NIU_RX0 = 6,
       C_NIU_RX1 = 7, C_NIU_ERR = 8, C_CORE_D_DENIED = 9 };
const char* kCounterNames[] = {"cycles", "retired", "fetch_stall", "data_stall", "tx0", "tx1", "rx0", "rx1",
                               "err", "core_d_denied"};
constexpr uint32_t kIoErrors = 0x114;
constexpr uint32_t kIoRxFlits = 0x11C;

#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      fprintf(stderr, "CHECK FAILED: " __VA_ARGS__);   \
      fprintf(stderr, "\n");                           \
      status = 1;                                      \
    }                                                  \
  } while (0)

template <class F>
bool throws(F f) {
  try {
    f();
  } catch (const std::runtime_error&) {
    return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 5'000'000);
  const int nx = SimHost::nx(), ny = SimHost::ny(), n = nx * ny;
  if (args.positional.size() != 1 && args.positional.size() != (size_t)n) {
    fprintf(stderr, "usage: tb_soc <program.elf> (one for all tiles, or %d in tile order) [options]\n", n);
    return 2;
  }
  SimHost sim(argc, argv, args);
  int status = 0;
  uint64_t sum_tx = 0, run_cycles = 0;
  try {
    Device dev(sim);
    CHECK((int)dev.nx() == nx && (int)dev.ny() == ny, "I/O tile reports a %ux%u grid", dev.nx(), dev.ny());
    std::vector<Xy> grid = dev.compute_grid();

    // Registers of every tile over the NoC, and addresses that must fail
    for (Xy t : grid) {
      uint32_t coord = dev.read32(t, kTctrl + kTctrlCoord), ctrl = dev.read32(t, kTctrl + kTctrlCtrl);
      uint32_t g = dev.read32(t, kTctrl + kTctrlGrid);
      CHECK(coord == (t.y << 8 | t.x) && ctrl == 0 && g == ((uint32_t)ny << 24 | (uint32_t)ny << 8 | (uint32_t)nx),
            "tile (%u,%u): COORD %08x CTRL %08x GRID %08x", t.x, t.y, coord, ctrl, g);
    }
    CHECK(throws([&] { dev.read32(grid[0], 0x10000000); }), "NIU registers answered a remote read");
    CHECK(throws([&] { dev.read32(grid[0], 0x10003000); }), "the console answered a remote read");
    uint8_t beat[16];
    CHECK(nx < 2 || throws([&] { sim.read(1u << 28 | (uint32_t)ny << 24, beat, 1); }),
          "a router-only stop answered");

    // Load, read back, launch
    std::vector<std::string> elfs(n, args.positional[0]);
    if (args.positional.size() > 1) elfs = args.positional;
    Program prog = dev.load_kernels(elfs, grid);
    size_t bytes = 0;
    for (auto& s : prog.images[0].segments) bytes += s.bytes.size();
    printf("loaded %s: %zu bytes, entry 0x%x%s\n", elfs[0].c_str(), bytes, prog.images[0].entry,
           args.positional.size() == 1 && n > 1 ? " (every tile)" : "");
    if (!args.flag("no-cosim"))
      for (int i = 0; i < n; i++) sim.attach_cosim(i, prog.images[i]);
    if (!args.flag("no-verify")) {
      std::string bad = dev.verify(prog);
      CHECK(bad.empty(), "image readback: %s", bad.c_str());
    }
    uint32_t io_rx0 = dev.io_read32(kIoRxFlits), io_err0 = dev.io_read32(kIoErrors);
    uint64_t t0 = sim.cycles();
    dev.launch(prog);

    // Run until DONE counts every tile, then let the NoC drain
    while (dev.done_count() < (uint32_t)n && !sim.failed()) sim.run(100);
    run_cycles = sim.cycles() - t0;
    sim.run(300);
    uint32_t io_rx1 = dev.io_read32(kIoRxFlits), io_err1 = dev.io_read32(kIoErrors);
    dev.stop(prog);
    if (sim.failed()) status = 1;

    // Exit words against the console
    std::vector<int> codes = dev.exit_codes(prog);
    for (int i = 0; i < n; i++) {
      const SimTile& st = sim.tile(i);
      CHECK(st.exited && codes[i] == (int)(int16_t)st.exit_code, "tile %d,%d: exit word %d, console %s %d",
            i % nx, i / nx, codes[i], st.exited ? "exit" : "silent", st.exit_code);
    }

    // Counters: snapshot and live view agree with the cores stopped
    auto snap = dev.read_counters(grid);
    auto live = dev.read_live_counters(grid);
    uint64_t sum_rx = 0, sum_err = 0;
    for (int i = 0; i < n; i++) {
      const SimTile& st = sim.tile(i);
      const auto& c = snap[i];
      CHECK(c == live[i], "tile %d: snapshot differs from the live counters with the core stopped", i);
      printf("counters tile %d,%d:", i % nx, i / nx);
      for (size_t k = 0; k < c.size(); k++) printf(" %s=%llu", kCounterNames[k], (unsigned long long)c[k]);
      printf("\n");
      sum_tx += c[C_NIU_TX0] + c[C_NIU_TX1];
      sum_rx += c[C_NIU_RX0] + c[C_NIU_RX1];
      sum_err += c[C_NIU_ERR];
      CHECK(c[C_RETIRED] <= c[C_CYCLES], "tile %d: retired %llu > cycles %llu", i,
            (unsigned long long)c[C_RETIRED], (unsigned long long)c[C_CYCLES]);
      if (st.sw_cleared) {
        printf("note: tile %d cleared its counters; retired-vs-RVFI check skipped\n", i);
      } else {
        // RVFI reports a retirement one cycle after the core counts it, and
        // stopping the core (CTRL.run = 0) resets the RVFI register at once: the
        // instruction retiring in the stop cycle is counted but never reported.
        CHECK(c[C_RETIRED] == st.rvfi_count || c[C_RETIRED] == st.rvfi_count + 1,
              "tile %d: retired counter %llu != RVFI retirements %llu", i, (unsigned long long)c[C_RETIRED],
              (unsigned long long)st.rvfi_count);
      }
      if (st.cosim)
        printf("lockstep tile %d,%d: %llu instructions, %s\n", i % nx, i / nx, (unsigned long long)st.cosim_retired,
               st.cosim_failed ? "MISMATCH" : "0 mismatches");
      if (status == 0 && codes[i] != 0) status = codes[i];
    }
    // Every flit sent while the cores ran arrived at a tile or the I/O tile.
    // Each tile also retires one flit nobody sent while it runs: the host's
    // stop write. No NIU errors anywhere, the I/O tile included.
    uint64_t io_rx = io_rx1 - io_rx0;
    CHECK(sum_tx + (uint64_t)n == sum_rx + io_rx && sum_err == 0 && io_err1 == io_err0,
          "NoC: %llu flits injected, %llu retired by tiles (including %d stop writes), %llu by the I/O tile, "
          "%llu NIU errors, %u I/O errors",
          (unsigned long long)sum_tx, (unsigned long long)sum_rx, n, (unsigned long long)io_rx,
          (unsigned long long)sum_err, io_err1 - io_err0);
  } catch (const std::exception& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    status = 1;
  }
  if (sim.failed() && status == 0) status = 1;
  sim.finish();
  printf("tb_soc %dx%d: %s (%llu cycles, %llu flits)\n", nx, ny, status == 0 ? "PASS" : "FAIL",
         (unsigned long long)run_cycles, (unsigned long long)sum_tx);
  return status;
}
