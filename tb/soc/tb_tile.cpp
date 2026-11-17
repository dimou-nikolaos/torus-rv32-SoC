// Single-tile system testbench (M0).
//
//   tb_tile <program.elf> [--seed=N] [--cycles=MAX] [--trace=f.fst] [--no-cosim]
//           [--coord=X,Y] [--isa=rv32im_zicsr_zifencei_zicclsm] [--rvfi-trace]
//
// Flow: reset, backdoor-load the ELF and verify it by reading the scratchpad
// back, release CTRL.run through the remote register port, run to the console
// exit register with Spike in lockstep on every retired instruction, read the
// counter bank through the remote port and check it. Exit status is the
// program's exit code, or nonzero on any mismatch or timeout.
#include <svdpi.h>
#include <verilated.h>
#include <verilated_fst_c.h>

#include <memory>
#include <string>

#include "Vtb_tile_top.h"
#include "Vtb_tile_top__Dpi.h"
#include "elf_loader.h"
#include "spike_cosim.h"
#include "tb_args.h"

using namespace torus;

namespace {

constexpr uint32_t kSpmBytes = 64 * 1024;
constexpr uint32_t kTctrlCtrl = 0x10001004;
constexpr uint32_t kPerfBase = 0x10002000;
constexpr uint32_t kPerfSnap = 0x10002800;
constexpr uint32_t kPerfNum = 0x10002FF0;
constexpr uint32_t kPerfCmd = 0x10002FF4;
constexpr uint32_t kPerfCmdSnapshot = 1, kPerfCmdClear = 2;
const char* kCounterNames[] = {"cycles", "retired", "fetch_stall", "data_stall"};

struct Sim {
  bool exited = false;
  int exit_code = 0;
  std::string line;
  std::unique_ptr<SpikeCosim> cosim;
  bool cosim_failed = false;
  bool rvfi_trace = false;
  bool released = false;  // CTRL.run written: the core may retire from here on
  bool sw_cleared = false;  // the program cleared its own counters
  uint64_t rvfi_count = 0;
  int x = 0, y = 0;
} g;

}  // namespace

// ------------------------------------------------------------------ DPI
extern "C" void torus_console_putc(int x, int y, char c) {
  if (c == '\n') {
    printf("[tile %d,%d] %s\n", x, y, g.line.c_str());
    g.line.clear();
  } else {
    g.line += c;
  }
}

extern "C" void torus_sim_exit(int x, int y, int code) {
  if (!g.line.empty()) torus_console_putc(x, y, '\n');
  if (!g.exited) {
    g.exited = true;
    g.exit_code = code;
  }
}

extern "C" void torus_rvfi_retire(int hart, long long order, int pc_rdata, int pc_wdata, int insn,
                                  svBit trap, int exc_cause, svBit intr, int intr_cause, int rd_addr,
                                  int rd_wdata, int mem_addr, int mem_rmask, int mem_wmask,
                                  int mem_rdata, int mem_wdata, svBit mem_err) {
  (void)hart;
  // --x-initial-edge fires clocked blocks once at initialization, when the RVFI
  // registers still hold random power-up values; a core in reset retires nothing.
  if (!g.released) return;
  g.rvfi_count++;
  if (mem_wmask && (uint32_t)mem_addr == kPerfCmd && (mem_wdata & kPerfCmdClear)) g.sw_cleared = true;
  if (g.rvfi_trace)
    printf("rvfi %lld pc=%08x insn=%08x%s%s rd=x%d=%08x mem=%08x r%x w%x%s\n", order,
           (uint32_t)pc_rdata, (uint32_t)insn, trap ? " TRAP" : "", intr ? " INTR" : "", rd_addr,
           (uint32_t)rd_wdata, (uint32_t)mem_addr, mem_rmask, mem_wmask, mem_err ? " ERR" : "");
  if (!g.cosim || g.cosim_failed) return;
  RvfiRecord r{(uint64_t)order, (uint32_t)pc_rdata, (uint32_t)pc_wdata, (uint32_t)insn,
               trap != 0, (uint32_t)exc_cause, intr != 0, (uint32_t)intr_cause,
               (uint32_t)rd_addr, (uint32_t)rd_wdata,
               (uint32_t)mem_addr, (uint32_t)mem_rmask, (uint32_t)mem_wmask,
               (uint32_t)mem_rdata, (uint32_t)mem_wdata, mem_err != 0};
  if (!g.cosim->step(r)) {
    g.cosim_failed = true;
    fprintf(stderr, "LOCKSTEP MISMATCH: %s\n", g.cosim->error().c_str());
  }
}

// ----------------------------------------------------------- backdoor
static svScope bank_scope(int b) {
  std::string name = "TOP.tb_tile_top.u_tile.u_spm.g_bank[" + std::to_string(b) + "].u_bank";
  svScope s = svGetScopeFromName(name.c_str());
  if (!s) {
    fprintf(stderr, "no DPI scope %s\n", name.c_str());
    exit(2);
  }
  return s;
}

static void bd_write32(uint32_t addr, uint32_t data) {
  svSetScope(bank_scope((addr >> 4) & 3));
  spm_bank_bd_write((int)(addr >> 6), (int)((addr >> 2) & 3), (int)data);
}

static uint32_t bd_read32(uint32_t addr) {
  svSetScope(bank_scope((addr >> 4) & 3));
  return (uint32_t)spm_bank_bd_read((int)(addr >> 6), (int)((addr >> 2) & 3));
}

// --------------------------------------------------------------- main
int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 5'000'000);
  if (args.positional.size() != 1) {
    fprintf(stderr, "usage: tb_tile <program.elf> [options]\n");
    return 2;
  }
  std::string coord = args.opt("coord", "0,0");
  g.rvfi_trace = args.flag("rvfi-trace");
  sscanf(coord.c_str(), "%d,%d", &g.x, &g.y);

  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randReset(2);
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  ctx->commandArgs(argc, argv);
  auto dut = std::make_unique<Vtb_tile_top>(ctx.get());
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

  dut->coord_x_i = g.x;
  dut->coord_y_i = g.y;
  dut->rmt_req_i = 0;
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

  // Backdoor load and readback
  ElfImage elf = load_elf(args.positional[0]);
  if (!args.flag("no-cosim"))
    g.cosim = std::make_unique<SpikeCosim>(args.opt("isa", "rv32im_zicsr_zifencei_zicclsm"), elf.entry,
                                           (uint32_t)(g.y << 4 | g.x), kSpmBytes, true);
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
      bd_write32(seg.addr + (uint32_t)i, w);
    }
    if (g.cosim) g.cosim->write_mem(seg.addr, seg.bytes.data(), seg.bytes.size());
    loaded += seg.bytes.size();
  }
  for (auto& seg : elf.segments)
    for (size_t i = 0; i < seg.bytes.size(); i += 4) {
      uint32_t w;
      memcpy(&w, &seg.bytes[i], 4);
      uint32_t got = bd_read32(seg.addr + (uint32_t)i);
      if (got != w) {
        fprintf(stderr, "backdoor readback mismatch at 0x%x: 0x%08x expected 0x%08x\n",
                seg.addr + (uint32_t)i, got, w);
        return 1;
      }
    }
  printf("loaded %s: %zu bytes, entry 0x%x\n", args.positional[0].c_str(), loaded, elf.entry);

  // Remote register access through the tile's remote port
  auto rmt = [&](bool we, uint32_t addr, uint32_t wdata, uint32_t* rdata) -> bool {
    dut->rmt_req_i = 1;
    dut->rmt_we_i = we;
    dut->rmt_addr_i = addr;
    dut->rmt_wdata_i = wdata;
    tick();
    dut->rmt_req_i = 0;
    dut->eval();
    bool ok = dut->rmt_rvalid_o && !dut->rmt_err_o;
    if (rdata) *rdata = dut->rmt_rdata_o;
    tick();
    return ok;
  };

  // Remote side of tile control: COORD, CTRL readback, read-only and unmapped errors
  {
    uint32_t v = 0;
    bool ok = rmt(false, 0x10001000, 0, &v);
    if (!ok || v != (uint32_t)(g.y << 8 | g.x)) {
      fprintf(stderr, "remote COORD read: ok=%d value 0x%x\n", ok, v);
      return 1;
    }
    if (!rmt(false, kTctrlCtrl, 0, &v) || v != 0) {
      fprintf(stderr, "remote CTRL read before launch: 0x%x\n", v);
      return 1;
    }
    if (rmt(true, 0x10001000, 5, nullptr) || rmt(false, 0x10000000, 0, nullptr) ||
        rmt(false, 0x00000100, 0, nullptr)) {
      fprintf(stderr, "remote write to COORD or read of an unmapped address did not error\n");
      return 1;
    }
  }

  // Launch protocol: clear the counters, then release the core
  if (!rmt(true, kPerfCmd, kPerfCmdClear, nullptr) || rmt(true, kPerfBase, 0, nullptr)) {
    fprintf(stderr, "remote PERF_CMD write failed, or a write to a counter did not error\n");
    return 1;
  }
  g.released = true;
  if (!rmt(true, kTctrlCtrl, 1, nullptr)) {
    fprintf(stderr, "remote write to CTRL failed\n");
    return 1;
  }

  // Run
  uint64_t cycles = 0, drain = 0;
  while (cycles < args.cycles && !ctx->gotFinish() && !g.cosim_failed && !ctx->gotError()) {
    tick();
    cycles++;
    if (g.exited && ++drain > 64) break;  // let the exit store retire
  }
  // Stop the core, then read the counters remotely
  rmt(true, kTctrlCtrl, 0, nullptr);

  int status = 0;
  if (ctx->gotError()) status = 1;
  if (g.cosim_failed) status = 1;
  if (!g.exited) {
    fprintf(stderr, "TIMEOUT after %llu cycles\n", (unsigned long long)cycles);
    status = 1;
  }

  uint32_t num = 0;
  if (!rmt(false, kPerfNum, 0, &num)) {
    fprintf(stderr, "counter bank read failed\n");
    status = 1;
  }
  // Read the snapshot view; with the core stopped it must equal the live view.
  if (!rmt(true, kPerfCmd, kPerfCmdSnapshot, nullptr)) status = 1;
  std::vector<uint64_t> cnt(num);
  for (uint32_t i = 0; i < num; i++) {
    uint32_t lo = 0, hi = 0, llo = 0, lhi = 0;
    bool ok = rmt(false, kPerfSnap + 8 * i, 0, &lo) && rmt(false, kPerfSnap + 8 * i + 4, 0, &hi) &&
              rmt(false, kPerfBase + 8 * i, 0, &llo) && rmt(false, kPerfBase + 8 * i + 4, 0, &lhi);
    if (!ok) status = 1;
    cnt[i] = (uint64_t)hi << 32 | lo;
    if (cnt[i] != ((uint64_t)lhi << 32 | llo)) {
      fprintf(stderr, "counter %u: snapshot %llu != live %llu with the core stopped\n", i,
              (unsigned long long)cnt[i], (unsigned long long)((uint64_t)lhi << 32 | llo));
      status = 1;
    }
  }
  printf("counters:");
  for (uint32_t i = 0; i < num; i++) {
    if (i < 4) printf(" %s=%llu", kCounterNames[i], (unsigned long long)cnt[i]);
    else if (cnt[i]) printf(" c%u=%llu", i, (unsigned long long)cnt[i]);
  }
  printf("\n");
  if (num >= 4) {
    if (cnt[1] > cnt[0]) {
      fprintf(stderr, "counter check: retired %llu > cycles %llu\n", (unsigned long long)cnt[1],
              (unsigned long long)cnt[0]);
      status = 1;
    }
    if (g.sw_cleared) {
      printf("note: the program cleared its counters; retired-vs-RVFI check skipped\n");
    } else if (cnt[1] != g.rvfi_count) {
      fprintf(stderr, "counter check: retired counter %llu != RVFI retirements %llu\n",
              (unsigned long long)cnt[1], (unsigned long long)g.rvfi_count);
      status = 1;
    }
  }
  if (g.cosim)
    printf("lockstep: %llu instructions, %s\n", (unsigned long long)g.cosim->retired(),
           g.cosim_failed ? "MISMATCH" : "0 mismatches");

  if (tfp) tfp->close();
  dut->final();
  if (status == 0) status = g.exit_code;
  printf("tb_tile: %s (exit code %d, %llu cycles)\n", status == 0 ? "PASS" : "FAIL", g.exit_code,
         (unsigned long long)cycles);
  return status;
}
