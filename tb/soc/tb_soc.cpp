// SoC testbench: an NX x NY torus of tiles (M1: NoC0, backdoor loading).
//
//   tb_soc <program.elf> [more.elf ...] [--seed=N] [--cycles=MAX] [--trace=f.fst]
//          [--no-cosim] [--isa=...] [--rvfi-trace]
//
// One ELF runs on every tile (kernels use my_xy()), or one ELF per tile in
// tile order (n = y * NX + x). Flow per tile: backdoor-load and read back,
// remote checks of tile control, clear the counters, release CTRL.run. Then run
// with Spike in lockstep on every tile until all tiles have exited and the NoC
// is quiet, stop the cores, snapshot and read every counter bank, and check the
// counters per tile and across the grid (every flit injected was retired, no
// NIU errors). Exit status: nonzero on any mismatch or timeout, else the first
// nonzero program exit code.
#include <svdpi.h>
#include <verilated.h>
#include <verilated_fst_c.h>

#include <memory>
#include <string>
#include <vector>

#include "Vtb_soc_top.h"
#include "Vtb_soc_top__Dpi.h"
#include "elf_loader.h"
#include "spike_cosim.h"
#include "tb_args.h"

using namespace torus;

#ifndef SOC_NX
#define SOC_NX 1
#endif
#ifndef SOC_NY
#define SOC_NY 1
#endif

namespace {

constexpr int kNX = SOC_NX, kNY = SOC_NY, kN = kNX * kNY;
constexpr uint32_t kSpmBytes = 64 * 1024;
constexpr uint32_t kTctrlCoord = 0x10001000, kTctrlCtrl = 0x10001004;
constexpr uint32_t kPerfBase = 0x10002000, kPerfSnap = 0x10002800, kPerfNum = 0x10002FF0, kPerfCmd = 0x10002FF4;
constexpr uint32_t kPerfCmdSnapshot = 1, kPerfCmdClear = 2;
// Counter indices, with the selects the counter bank has after reset
enum { C_CYCLES = 0, C_RETIRED = 1, C_FETCH = 2, C_DATA = 3, C_NIU_TX = 4, C_NIU_RX = 5, C_NIU_ERR = 6,
       C_R0_FLIT = 7 };
const char* kCounterNames[] = {"cycles", "retired", "fetch_stall", "data_stall"};

struct Tile {
  bool exited = false;
  int exit_code = 0;
  std::string line;
  std::unique_ptr<SpikeCosim> cosim;
  bool cosim_failed = false;
  bool sw_cleared = false;
  uint64_t rvfi_count = 0;
};

struct Sim {
  Tile tile[kN];
  bool released = false;  // CTRL.run written: cores may retire from here on
  bool rvfi_trace = false;
  bool any_failed = false;
} g;

int tile_of_hart(int hart) { return (hart >> 4) * kNX + (hart & 15); }

// Packed per-tile port helpers for 1..N tiles (Verilator picks the C type by width)
void set_word(uint32_t& v, int, uint32_t w) { v = w; }
void set_word(uint64_t& v, int n, uint32_t w) {
  v = (v & ~(0xffffffffull << (32 * n))) | ((uint64_t)w << (32 * n));
}
template <std::size_t W>
void set_word(VlWide<W>& v, int n, uint32_t w) { v[n] = w; }
uint32_t get_word(uint32_t v, int) { return v; }
uint32_t get_word(uint64_t v, int n) { return (uint32_t)(v >> (32 * n)); }
template <std::size_t W>
uint32_t get_word(const VlWide<W>& v, int n) { return v[n]; }
template <typename T>
void set_bit(T& v, int n, bool b) {
  if (b) v |= (T)1 << n;
  else v &= ~((T)1 << n);
}

}  // namespace

// ------------------------------------------------------------------ DPI
extern "C" void torus_console_putc(int x, int y, char c) {
  Tile& t = g.tile[y * kNX + x];
  if (c == '\n') {
    printf("[tile %d,%d] %s\n", x, y, t.line.c_str());
    t.line.clear();
  } else {
    t.line += c;
  }
}

extern "C" void torus_sim_exit(int x, int y, int code) {
  Tile& t = g.tile[y * kNX + x];
  if (!t.line.empty()) torus_console_putc(x, y, '\n');
  if (!t.exited) {
    t.exited = true;
    t.exit_code = code;
  }
}

extern "C" void torus_rvfi_retire(int hart, long long order, int pc_rdata, int pc_wdata, int insn,
                                  svBit trap, int exc_cause, svBit intr, int intr_cause, int rd_addr,
                                  int rd_wdata, int mem_addr, int mem_rmask, int mem_wmask,
                                  int mem_rdata, int mem_wdata, svBit mem_err) {
  // --x-initial-edge fires clocked blocks once at initialization, when the RVFI
  // registers still hold random power-up values; a core in reset retires nothing.
  if (!g.released) return;
  Tile& t = g.tile[tile_of_hart(hart)];
  t.rvfi_count++;
  if (mem_wmask && (uint32_t)mem_addr == kPerfCmd && (mem_wdata & kPerfCmdClear)) t.sw_cleared = true;
  if (g.rvfi_trace)
    printf("rvfi %d,%d %lld pc=%08x insn=%08x%s%s rd=x%d=%08x mem=%08x r%x w%x%s\n", hart & 15, hart >> 4,
           order, (uint32_t)pc_rdata, (uint32_t)insn, trap ? " TRAP" : "", intr ? " INTR" : "", rd_addr,
           (uint32_t)rd_wdata, (uint32_t)mem_addr, mem_rmask, mem_wmask, mem_err ? " ERR" : "");
  if (!t.cosim || t.cosim_failed) return;
  RvfiRecord r{(uint64_t)order, (uint32_t)pc_rdata, (uint32_t)pc_wdata, (uint32_t)insn,
               trap != 0, (uint32_t)exc_cause, intr != 0, (uint32_t)intr_cause,
               (uint32_t)rd_addr, (uint32_t)rd_wdata,
               (uint32_t)mem_addr, (uint32_t)mem_rmask, (uint32_t)mem_wmask,
               (uint32_t)mem_rdata, (uint32_t)mem_wdata, mem_err != 0};
  if (!t.cosim->step(r)) {
    t.cosim_failed = true;
    g.any_failed = true;
    fprintf(stderr, "LOCKSTEP MISMATCH on tile %d,%d: %s\n", hart & 15, hart >> 4, t.cosim->error().c_str());
  }
}

// ----------------------------------------------------------- backdoor
static svScope bank_scope(int n, int b) {
  std::string name = "TOP.tb_soc_top.u_soc.g_y[" + std::to_string(n / kNX) + "].g_x[" + std::to_string(n % kNX) +
                     "].g_tile.u_tile.u_spm.g_bank[" + std::to_string(b) + "].u_bank";
  svScope s = svGetScopeFromName(name.c_str());
  if (!s) {
    fprintf(stderr, "no DPI scope %s\n", name.c_str());
    exit(2);
  }
  return s;
}

static void bd_write32(int n, uint32_t addr, uint32_t data) {
  svSetScope(bank_scope(n, (addr >> 4) & 3));
  spm_bank_bd_write((int)(addr >> 6), (int)((addr >> 2) & 3), (int)data);
}

static uint32_t bd_read32(int n, uint32_t addr) {
  svSetScope(bank_scope(n, (addr >> 4) & 3));
  return (uint32_t)spm_bank_bd_read((int)(addr >> 6), (int)((addr >> 2) & 3));
}

// --------------------------------------------------------------- main
int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 5'000'000);
  if (args.positional.size() != 1 && args.positional.size() != (size_t)kN) {
    fprintf(stderr, "usage: tb_soc <program.elf> (one for all tiles, or %d in tile order) [options]\n", kN);
    return 2;
  }
  g.rvfi_trace = args.flag("rvfi-trace");

  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randReset(2);
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  ctx->commandArgs(argc, argv);
  auto dut = std::make_unique<Vtb_soc_top>(ctx.get());
  std::unique_ptr<VerilatedFstC> tfp;
  if (!args.trace.empty()) {
    ctx->traceEverOn(true);
    tfp = std::make_unique<VerilatedFstC>();
    dut->trace(tfp.get(), 99);
    tfp->open(args.trace.c_str());
  }
  auto tick = [&]() {
    dut->clk_i = 0;
    dut->eval();
    if (tfp) tfp->dump(ctx->time());
    ctx->timeInc(1);
    dut->clk_i = 1;
    dut->eval();
    if (tfp) tfp->dump(ctx->time());
    ctx->timeInc(1);
  };

  dut->tb_req_i = 0;
  // Flops start random (--x-initial unique); assertions are meaningless until
  // the asynchronous reset has been applied.
  ctx->assertOn(false);
  dut->rst_ni = 1;
  dut->eval();
  dut->rst_ni = 0;
  for (int i = 0; i < 5; i++) tick();
  dut->rst_ni = 1;
  tick();
  ctx->assertOn(true);

  // Backdoor load and readback, per tile
  // The NIU writes the scratchpad (remote tiles, loopback), and programs may read
  // bytes they never wrote: scratchpad loads take the RTL value (SPEC.md, the
  // Spike sync rule) instead of being predicted from a mirror.
  bool shared = true;
  for (int n = 0; n < kN; n++) {
    const std::string& path = args.positional.size() == 1 ? args.positional[0] : args.positional[n];
    ElfImage elf = load_elf(path);
    Tile& t = g.tile[n];
    if (!args.flag("no-cosim"))
      t.cosim = std::make_unique<SpikeCosim>(args.opt("isa", "rv32im_zicsr_zifencei_zicclsm"), elf.entry,
                                             (uint32_t)((n / kNX) << 4 | (n % kNX)), kSpmBytes, !shared);
    size_t loaded = 0;
    for (auto& seg : elf.segments) {
      if (seg.addr + seg.bytes.size() > kSpmBytes) {
        fprintf(stderr, "segment at 0x%x does not fit the scratchpad\n", seg.addr);
        return 2;
      }
      seg.bytes.resize((seg.bytes.size() + 3) & ~size_t(3), 0);
      for (size_t i = 0; i < seg.bytes.size(); i += 4) {
        uint32_t w;
        memcpy(&w, &seg.bytes[i], 4);
        bd_write32(n, seg.addr + (uint32_t)i, w);
      }
      if (t.cosim) t.cosim->write_mem(seg.addr, seg.bytes.data(), seg.bytes.size());
      loaded += seg.bytes.size();
    }
    for (auto& seg : elf.segments)
      for (size_t i = 0; i < seg.bytes.size(); i += 4) {
        uint32_t w;
        memcpy(&w, &seg.bytes[i], 4);
        uint32_t got = bd_read32(n, seg.addr + (uint32_t)i);
        if (got != w) {
          fprintf(stderr, "tile %d: backdoor readback mismatch at 0x%x: 0x%08x expected 0x%08x\n", n,
                  seg.addr + (uint32_t)i, got, w);
          return 1;
        }
      }
    if (n == 0 || args.positional.size() > 1)
      printf("loaded %s: %zu bytes, entry 0x%x%s\n", path.c_str(), loaded, elf.entry,
             args.positional.size() == 1 && kN > 1 ? " (every tile)" : "");
  }

  // Testbench register access to one tile
  auto rmt = [&](int n, bool we, uint32_t addr, uint32_t wdata, uint32_t* rdata) -> bool {
    set_bit(dut->tb_req_i, n, true);
    set_bit(dut->tb_we_i, n, we);
    set_word(dut->tb_addr_i, n, addr);
    set_word(dut->tb_wdata_i, n, wdata);
    tick();
    set_bit(dut->tb_req_i, n, false);
    dut->eval();
    bool ok = (dut->tb_rvalid_o >> n & 1) && !(dut->tb_err_o >> n & 1);
    if (rdata) *rdata = get_word(dut->tb_rdata_o, n);
    tick();
    return ok;
  };

  for (int n = 0; n < kN; n++) {
    int x = n % kNX, y = n / kNX;
    uint32_t v = 0;
    bool ok = rmt(n, false, kTctrlCoord, 0, &v);
    if (!ok || v != (uint32_t)(y << 8 | x)) {
      fprintf(stderr, "tile %d: remote COORD read: ok=%d value 0x%x\n", n, ok, v);
      return 1;
    }
    if (!rmt(n, false, kTctrlCtrl, 0, &v) || v != 0) {
      fprintf(stderr, "tile %d: remote CTRL read before launch: 0x%x\n", n, v);
      return 1;
    }
    if (rmt(n, true, kTctrlCoord, 5, nullptr) || rmt(n, false, 0x10000000, 0, nullptr) ||
        rmt(n, false, 0x00000100, 0, nullptr)) {
      fprintf(stderr, "tile %d: remote write to COORD or read of an unmapped address did not error\n", n);
      return 1;
    }
    // Launch protocol: clear the counters
    if (!rmt(n, true, kPerfCmd, kPerfCmdClear, nullptr) || rmt(n, true, kPerfBase, 0, nullptr)) {
      fprintf(stderr, "tile %d: PERF_CMD write failed, or a write to a counter did not error\n", n);
      return 1;
    }
  }
  // Release every core in the same cycle
  g.released = true;
  for (int n = 0; n < kN; n++) {
    set_bit(dut->tb_req_i, n, true);
    set_bit(dut->tb_we_i, n, true);
    set_word(dut->tb_addr_i, n, kTctrlCtrl);
    set_word(dut->tb_wdata_i, n, 1);
  }
  tick();
  dut->tb_req_i = 0;
  tick();

  // Run until every tile has exited, then let the exit stores retire and the NoC drain
  auto all_exited = [&]() {
    for (auto& t : g.tile) if (!t.exited) return false;
    return true;
  };
  uint64_t cycles = 0, drain = 0;
  while (cycles < args.cycles && !ctx->gotFinish() && !g.any_failed && !ctx->gotError()) {
    tick();
    cycles++;
    if (all_exited() && ++drain > 300) break;
  }
  for (int n = 0; n < kN; n++) rmt(n, true, kTctrlCtrl, 0, nullptr);  // stop the cores

  int status = 0;
  if (ctx->gotError() || g.any_failed) status = 1;
  for (int n = 0; n < kN; n++)
    if (!g.tile[n].exited) {
      fprintf(stderr, "TIMEOUT: tile %d,%d did not exit within %llu cycles\n", n % kNX, n / kNX,
              (unsigned long long)cycles);
      status = 1;
    }

  // Counters: snapshot, read, check against the live view and the lockstep
  uint64_t sum_tx = 0, sum_rx = 0, sum_err = 0;
  for (int n = 0; n < kN; n++) {
    Tile& t = g.tile[n];
    uint32_t num = 0;
    if (!rmt(n, false, kPerfNum, 0, &num) || !rmt(n, true, kPerfCmd, kPerfCmdSnapshot, nullptr)) {
      fprintf(stderr, "tile %d: counter bank access failed\n", n);
      status = 1;
      continue;
    }
    std::vector<uint64_t> cnt(num);
    for (uint32_t i = 0; i < num; i++) {
      uint32_t lo = 0, hi = 0, llo = 0, lhi = 0;
      bool ok = rmt(n, false, kPerfSnap + 8 * i, 0, &lo) && rmt(n, false, kPerfSnap + 8 * i + 4, 0, &hi) &&
                rmt(n, false, kPerfBase + 8 * i, 0, &llo) && rmt(n, false, kPerfBase + 8 * i + 4, 0, &lhi);
      if (!ok) status = 1;
      cnt[i] = (uint64_t)hi << 32 | lo;
      if (cnt[i] != ((uint64_t)lhi << 32 | llo)) {
        fprintf(stderr, "tile %d counter %u: snapshot %llu != live with the core stopped\n", n, i,
                (unsigned long long)cnt[i]);
        status = 1;
      }
    }
    printf("counters tile %d,%d:", n % kNX, n / kNX);
    for (uint32_t i = 0; i < num; i++) {
      if (i < 4) printf(" %s=%llu", kCounterNames[i], (unsigned long long)cnt[i]);
      else if (cnt[i]) printf(" c%u=%llu", i, (unsigned long long)cnt[i]);
    }
    printf("\n");
    if (num > C_NIU_ERR) {
      sum_tx += cnt[C_NIU_TX];
      sum_rx += cnt[C_NIU_RX];
      sum_err += cnt[C_NIU_ERR];
      // Every flit the NIU retired passed this router's ejection output
      if (cnt[C_R0_FLIT] != cnt[C_NIU_RX]) {
        fprintf(stderr, "tile %d: router ejected %llu flits, NIU retired %llu\n", n,
                (unsigned long long)cnt[C_R0_FLIT], (unsigned long long)cnt[C_NIU_RX]);
        status = 1;
      }
    }
    if (cnt[C_RETIRED] > cnt[C_CYCLES]) {
      fprintf(stderr, "tile %d: retired %llu > cycles %llu\n", n, (unsigned long long)cnt[C_RETIRED],
              (unsigned long long)cnt[C_CYCLES]);
      status = 1;
    }
    if (t.sw_cleared) {
      printf("note: tile %d cleared its counters; retired-vs-RVFI check skipped\n", n);
    } else if (cnt[C_RETIRED] != t.rvfi_count && cnt[C_RETIRED] != t.rvfi_count + 1) {
      // RVFI reports a retirement one cycle after the core counts it, and
      // stopping the core (CTRL.run = 0) resets the RVFI register at once: the
      // instruction retiring in the stop cycle is counted but never reported.
      fprintf(stderr, "tile %d: retired counter %llu != RVFI retirements %llu\n", n,
              (unsigned long long)cnt[C_RETIRED], (unsigned long long)t.rvfi_count);
      status = 1;
    }
    if (t.cosim)
      printf("lockstep tile %d,%d: %llu instructions, %s\n", n % kNX, n / kNX, (unsigned long long)t.cosim->retired(),
             t.cosim_failed ? "MISMATCH" : "0 mismatches");
    if (status == 0 && t.exit_code != 0) status = t.exit_code;
  }
  if (sum_tx != sum_rx || sum_err != 0) {
    fprintf(stderr, "NoC: %llu flits injected, %llu retired, %llu NIU errors\n", (unsigned long long)sum_tx,
            (unsigned long long)sum_rx, (unsigned long long)sum_err);
    if (status == 0) status = 1;
  }

  if (tfp) tfp->close();
  dut->final();
  printf("tb_soc %dx%d: %s (%llu cycles, %llu flits)\n", kNX, kNY, status == 0 ? "PASS" : "FAIL",
         (unsigned long long)cycles, (unsigned long long)sum_tx);
  return status;
}
