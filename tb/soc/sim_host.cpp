// SimHost (see sim_host.h).
#include "sim_host.h"

#include <svdpi.h>
#include <verilated.h>

#include "tb_trace.h"

#include <chrono>
#include <stdexcept>

#include "Vtb_soc_top.h"
#include "Vtb_soc_top__Dpi.h"
#include "axi_bfm.h"
#include "spike_cosim.h"
#include "spm_model.h"

#ifndef SOC_NX
#define SOC_NX 1
#endif
#ifndef SOC_NY
#define SOC_NY 1
#endif

namespace torus {

namespace {
constexpr int kNX = SOC_NX, kNY = SOC_NY, kN = kNX * kNY;
}  // namespace

struct SimHost::Impl {
  std::unique_ptr<VerilatedContext> ctx;
  std::unique_ptr<Vtb_soc_top> dut;
  TbTrace tfp;
  std::unique_ptr<AxiMaster<Vtb_soc_top>> bfm;
  int ah = 4, fh = 5;
  uint64_t now = 0, anext = 1, fnext = 3, fcycles = 0, acycles = 0, max_cycles = 0;
  bool released = false, rvfi_trace = false, any_failed = false;
  std::string isa;
  SimTile tiles[kN];
  std::unique_ptr<SpikeCosim> cosim[kN];
  std::string line[kN];
  std::vector<int> pending_writes;
  std::chrono::steady_clock::time_point t_start = std::chrono::steady_clock::now();

  void eval() {
    dut->eval();
    tfp.dump(now);
  }
  // Advance to the next clock edge(s); the AXI model acts on AXI rising edges
  void step() {
    uint64_t t = std::min(anext, fnext);
    now = t;
    bool a = anext == t, f = fnext == t;
    bool arise = a && !dut->aclk_i;
    if (arise) bfm->drive();
    eval();
    if (arise && !bfm->sample()) throw std::runtime_error("AXI protocol: " + bfm->error());
    if (a) {
      dut->aclk_i = !dut->aclk_i;
      anext += (uint64_t)ah;
      if (arise) acycles++;
    }
    if (f) {
      if (!dut->clk_i) fcycles++;
      dut->clk_i = !dut->clk_i;
      fnext += (uint64_t)fh;
    }
    eval();
    if (ctx->gotError() && !any_failed) any_failed = true;
    if (max_cycles && fcycles > max_cycles)
      throw std::runtime_error("timeout after " + std::to_string(fcycles) + " fabric cycles");
  }
  void wait_for(int h) {
    while (!bfm->done(h)) step();
  }
};

static SimHost::Impl* g_sim = nullptr;

SimHost::SimHost(int argc, char** argv, const TbArgs& args) : impl_(std::make_unique<Impl>()) {
  Impl& s = *impl_;
  g_sim = &s;
  s.ah = std::stoi(args.opt("axi-half", "4"));
  s.fh = std::stoi(args.opt("fabric-half", "5"));
  s.max_cycles = args.cycles;
  s.rvfi_trace = args.flag("rvfi-trace");
  s.isa = args.opt("isa", "rv32im_zicsr_zifencei_zicclsm");
  s.ctx = std::make_unique<VerilatedContext>();
  s.ctx->randReset(2);
  s.ctx->randSeed((int)(args.seed & 0x7fffffff));
  s.ctx->commandArgs(argc, argv);
  s.dut = std::make_unique<Vtb_soc_top>(s.ctx.get());
  s.tfp.open(s.ctx.get(), s.dut.get(), args.trace);
  s.bfm = std::make_unique<AxiMaster<Vtb_soc_top>>(s.dut.get(), args.seed ^ 0xa11);
  // Both domains in reset; flops start random (--x-initial unique), so
  // assertions stay off until the asynchronous resets have been applied.
  s.ctx->assertOn(false);
  s.dut->clk_i = 0;
  s.dut->aclk_i = 0;
  s.dut->rst_ni = 1;
  s.dut->arst_ni = 1;
  s.eval();
  s.dut->rst_ni = 0;
  s.dut->arst_ni = 0;
  s.eval();
  for (int i = 0; i < 40; i++) s.step();
  s.dut->rst_ni = 1;
  s.dut->arst_ni = 1;
  s.eval();
  s.ctx->assertOn(true);
  s.fcycles = 0;
  s.acycles = 0;
  // From here on a core retires only once the host sets its CTRL.run
  s.released = true;
}

SimHost::~SimHost() { g_sim = nullptr; }

int SimHost::nx() { return kNX; }
int SimHost::ny() { return kNY; }

void SimHost::write(uint32_t addr, const uint8_t* data, const uint16_t* strb, unsigned beats) {
  std::vector<Beat> b(beats);
  std::vector<uint16_t> s(strb, strb + beats);
  for (unsigned i = 0; i < beats; i++) memcpy(b[i].w, data + 16 * i, 16);
  impl_->pending_writes.push_back(impl_->bfm->write(addr, b, s));
  // Posted, but keep the queue short
  if (impl_->pending_writes.size() > 16) sync();
}

void SimHost::sync() {
  Impl& s = *impl_;
  for (int h : s.pending_writes) {
    s.wait_for(h);
    uint32_t resp = s.bfm->result(h).resp;
    s.bfm->forget(h);
    if (resp) {
      s.pending_writes.clear();
      throw std::runtime_error("AXI write answered SLVERR");
    }
  }
  s.pending_writes.clear();
}

void SimHost::read(uint32_t addr, uint8_t* data, unsigned beats) {
  Impl& s = *impl_;
  int h = s.bfm->read(addr, beats);
  s.wait_for(h);
  const AxiResult& r = s.bfm->result(h);
  uint32_t resp = r.resp;
  for (unsigned i = 0; i < beats && i < r.data.size(); i++) memcpy(data + 16 * i, r.data[i].w, 16);
  s.bfm->forget(h);
  if (resp) throw std::runtime_error("AXI read answered SLVERR");
}

void SimHost::run(uint64_t n) {
  uint64_t end = impl_->fcycles + n;
  while (impl_->fcycles < end) impl_->step();
}

uint64_t SimHost::cycles() const { return impl_->fcycles; }
uint64_t SimHost::axi_cycles() const { return impl_->acycles; }

void SimHost::attach_cosim(int n, const ElfImage& img) {
  Impl& s = *impl_;
  // The NIU writes the scratchpad (remote tiles, loopback), and programs may
  // read bytes they never wrote: scratchpad loads take the RTL value (SPEC.md,
  // the Spike sync rule) instead of being predicted from a mirror.
  s.cosim[n] = std::make_unique<SpikeCosim>(s.isa, img.entry, (uint32_t)((n / kNX) << 4 | (n % kNX)), kSpmBytes,
                                            false);
  for (auto& seg : img.segments) s.cosim[n]->write_mem(seg.addr, seg.bytes.data(), seg.bytes.size());
  s.tiles[n].cosim = true;
}

const SimTile& SimHost::tile(int n) const {
  SimTile& t = impl_->tiles[n];
  if (impl_->cosim[n]) t.cosim_retired = impl_->cosim[n]->retired();
  return t;
}

bool SimHost::all_exited() const {
  for (auto& t : impl_->tiles) if (!t.exited) return false;
  return true;
}

bool SimHost::failed() const { return impl_->any_failed || impl_->ctx->gotError(); }

void SimHost::finish() {
  Impl& s = *impl_;
  s.tfp.close();
  s.dut->final();
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t_start).count();
  printf("simulation: %llu fabric cycles in %.1f s (%.2f kHz, %u Verilator threads, lockstep %s)\n",
         (unsigned long long)s.fcycles, wall, s.fcycles / wall / 1e3, s.dut->threads(),
         s.cosim[0] ? "on" : "off");
}

}  // namespace torus

// ------------------------------------------------------------------ DPI
using torus::g_sim;

extern "C" void torus_console_putc(int x, int y, char c) {
  std::string& l = g_sim->line[y * torus::kNX + x];
  if (c == '\n') {
    printf("[tile %d,%d] %s\n", x, y, l.c_str());
    l.clear();
  } else {
    l += c;
  }
}

extern "C" void torus_sim_exit(int x, int y, int code) {
  torus::SimTile& t = g_sim->tiles[y * torus::kNX + x];
  if (!g_sim->line[y * torus::kNX + x].empty()) torus_console_putc(x, y, '\n');
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
  if (!g_sim || !g_sim->released) return;
  int n = (hart >> 4) * torus::kNX + (hart & 15);
  torus::SimTile& t = g_sim->tiles[n];
  t.rvfi_count++;
  if (mem_wmask && (uint32_t)mem_addr == torus::kPerf + torus::kPerfCmd && (mem_wdata & torus::kPerfClear))
    t.sw_cleared = true;
  if (g_sim->rvfi_trace)
    printf("rvfi %d,%d %lld pc=%08x insn=%08x%s%s rd=x%d=%08x mem=%08x r%x w%x%s\n", hart & 15, hart >> 4,
           order, (uint32_t)pc_rdata, (uint32_t)insn, trap ? " TRAP" : "", intr ? " INTR" : "", rd_addr,
           (uint32_t)rd_wdata, (uint32_t)mem_addr, mem_rmask, mem_wmask, mem_err ? " ERR" : "");
  auto& cs = g_sim->cosim[n];
  if (!cs || t.cosim_failed) return;
  torus::RvfiRecord r{(uint64_t)order, (uint32_t)pc_rdata, (uint32_t)pc_wdata, (uint32_t)insn,
                      trap != 0, (uint32_t)exc_cause, intr != 0, (uint32_t)intr_cause,
                      (uint32_t)rd_addr, (uint32_t)rd_wdata,
                      (uint32_t)mem_addr, (uint32_t)mem_rmask, (uint32_t)mem_wmask,
                      (uint32_t)mem_rdata, (uint32_t)mem_wdata, mem_err != 0};
  if (!cs->step(r)) {
    t.cosim_failed = true;
    g_sim->any_failed = true;
    fprintf(stderr, "LOCKSTEP MISMATCH on tile %d,%d: %s\n", hart & 15, hart >> 4, cs->error().c_str());
  }
}
