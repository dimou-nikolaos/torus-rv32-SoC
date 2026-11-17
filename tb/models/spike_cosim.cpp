#include "spike_cosim.h"

#include <cstdio>
#include <cstring>

#include "riscv/decode.h"
#include "riscv/encoding.h"
#include "riscv/mmu.h"
#include "riscv/processor.h"

namespace torus {

namespace {

std::string hex(uint64_t v) {
  char b[24];
  snprintf(b, sizeof b, "0x%08llx", (unsigned long long)v);
  return b;
}

// CSRs whose values Spike cannot know (free-running counters) or which are
// implementation-defined in CV32E40X. Reads of these take the RTL value.
bool csr_synced(uint32_t csr) {
  switch (csr) {
    case CSR_MCYCLE: case CSR_MCYCLEH: case CSR_CYCLE: case CSR_CYCLEH:
    case CSR_TIME: case CSR_TIMEH:
    case CSR_MISA: case CSR_MVENDORID: case CSR_MARCHID: case CSR_MIMPID:
    case CSR_MHARTID: case CSR_MCONFIGPTR:
      return true;
    default:
      return (csr >= CSR_MHPMCOUNTER3 && csr <= CSR_MHPMCOUNTER31) ||
             (csr >= CSR_MHPMCOUNTER3H && csr <= CSR_MHPMCOUNTER31H) ||
             (csr >= CSR_HPMCOUNTER3 && csr <= CSR_HPMCOUNTER31) ||
             (csr >= CSR_HPMCOUNTER3H && csr <= CSR_HPMCOUNTER31H);
  }
}

bool is_csr_read(uint32_t insn, uint32_t* csr) {
  uint32_t opcode = insn & 0x7f, funct3 = (insn >> 12) & 7, rd = (insn >> 7) & 31;
  if (opcode != 0x73 || funct3 == 0 || funct3 == 4 || rd == 0) return false;
  *csr = insn >> 20;
  return true;
}

}  // namespace

SpikeCosim::SpikeCosim(const std::string& isa, uint32_t start_pc, uint32_t hartid,
                       uint32_t spm_bytes, bool check_spm_loads)
    : spm_(spm_bytes, 0), check_spm_loads_(check_spm_loads) {
  cfg_.isa = isa.c_str();
  cfg_.priv = "M";
  cfg_.pmpregions = 0;
  cfg_.hartids = {hartid};
  cfg_.trigger_count = 0;
  null_log_ = fopen("/dev/null", "w");
  proc_ = std::make_unique<processor_t>(isa.c_str(), "M", &cfg_, this, hartid, false, null_log_,
                                        sout_);
  harts_[hartid] = proc_.get();
  proc_->enable_log_commits();
  proc_->reset();
  proc_->get_state()->pc = start_pc;
}

SpikeCosim::~SpikeCosim() {
  proc_.reset();
  if (null_log_) fclose(null_log_);
}

void SpikeCosim::write_mem(uint32_t addr, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++)
    if (addr + i < spm_.size()) spm_[addr + i] = data[i];
}

bool SpikeCosim::fail(const std::string& msg) {
  if (error_.empty()) error_ = msg;
  return false;
}

std::string SpikeCosim::where(const RvfiRecord& r) const {
  return "order " + std::to_string(r.order) + " pc " + hex(r.pc_rdata) + " insn " + hex(r.insn) +
         ": ";
}

bool SpikeCosim::mmio_fetch(reg_t paddr, size_t len, uint8_t* bytes) {
  if (paddr + len > spm_.size()) return false;
  memcpy(bytes, &spm_[paddr], len);
  return true;
}

bool SpikeCosim::mmio_load(reg_t paddr, size_t len, uint8_t* bytes) {
  const RvfiRecord* r = cur_;
  mem_ops_++;
  if (!r) return false;
  if (!r->mem_rmask) {
    mem_failed_ = true;
    return fail(where(*r) + "Spike loads " + hex(paddr) + " but the RTL did no load");
  }
  // Spike splits misaligned accesses into byte callbacks: accept any part of the
  // RTL access, at its offset from the RVFI address.
  uint32_t off = (uint32_t)(paddr - r->mem_addr);
  if (paddr < r->mem_addr || off + len > 4 || !((r->mem_rmask >> off) & 1)) {
    mem_failed_ = true;
    return fail(where(*r) + "load address: RTL " + hex(r->mem_addr) + " mask " +
                hex(r->mem_rmask) + ", Spike " + hex(paddr) + " size " + std::to_string(len));
  }
  if (r->mem_err) nmi_pending_ = true;  // bus error: the load completes, an NMI follows
  // RVFI rdata is the register-format value, relative to the access address.
  for (size_t i = 0; i < len; i++) bytes[i] = (uint8_t)(r->mem_rdata >> (8 * (off + i)));
  if (check_spm_loads_ && paddr + len <= spm_.size()) {
    for (size_t i = 0; i < len; i++)
      if (spm_[paddr + i] != bytes[i]) {
        mem_failed_ = true;
        return fail(where(*r) + "scratchpad load " + hex(paddr + i) + ": RTL byte " +
                    hex(bytes[i]) + " expected " + hex(spm_[paddr + i]));
      }
  }
  return true;
}

bool SpikeCosim::mmio_store(reg_t paddr, size_t len, const uint8_t* bytes) {
  const RvfiRecord* r = cur_;
  mem_ops_++;
  if (!r) return false;
  if (!r->mem_wmask) {
    mem_failed_ = true;
    return fail(where(*r) + "Spike stores " + hex(paddr) + " but the RTL did no store");
  }
  // RVFI reports store data and masks relative to the access address (the
  // vendor RVFI rotates OBI write data back), like load data.
  uint32_t off = (uint32_t)(paddr - r->mem_addr);
  uint32_t want = ((1u << len) - 1) << off;
  if (paddr < r->mem_addr || off + len > 4 || (r->mem_wmask & want) != want) {
    mem_failed_ = true;
    return fail(where(*r) + "store: RTL addr " + hex(r->mem_addr) + " mask " + hex(r->mem_wmask) +
                ", Spike " + hex(paddr) + " size " + std::to_string(len));
  }
  for (size_t i = 0; i < len; i++) {
    uint8_t rtl = (uint8_t)(r->mem_wdata >> (8 * (off + i)));
    if (rtl != bytes[i]) {
      mem_failed_ = true;
      return fail(where(*r) + "store data byte " + std::to_string(i) + ": RTL " + hex(rtl) +
                  " Spike " + hex(bytes[i]));
    }
  }
  if (r->mem_err) {
    nmi_pending_ = true;
    return true;
  }
  write_mem((uint32_t)paddr, bytes, len);
  return true;
}

bool SpikeCosim::step(const RvfiRecord& r) {
  state_t* s = proc_->get_state();
  if (!r.intr && (uint32_t)s->pc != r.pc_rdata)
    return fail(where(r) + "PC: RTL " + hex(r.pc_rdata) + " Spike " + hex(s->pc));
  if (r.intr) {
    // Only bus-error NMIs exist at M0 (no interrupt sources are connected).
    if (r.intr_cause != 1024 && r.intr_cause != 1025)
      return fail(where(r) + "unexpected interrupt, cause " + std::to_string(r.intr_cause));
    if (!nmi_pending_)
      return fail(where(r) + "RTL took a bus-error NMI but no access reported an error");
    nmi_pending_ = false;
    // Direct mode (the only mode the SDK uses): the NMI enters at the mtvec base,
    // which CV32E40X aligns to 128 bytes.
    reg_t vec = s->mtvec->read() & ~(reg_t)0x7f;
    if ((s->mtvec->read() & 3) != 0 || r.pc_rdata != (uint32_t)vec)
      return fail(where(r) + "NMI entered at " + hex(r.pc_rdata) + ", expected mtvec base " + hex(vec));
    // NMI entry as CV32E40X performs it: mepc = interrupted PC, mcause with
    // the interrupt bit, MPP = M, MPIE = MIE, MIE = 0, then the handler.
    reg_t mstatus = s->mstatus->read();
    reg_t mie = (mstatus >> 3) & 1;
    mstatus = (mstatus & ~(reg_t)(MSTATUS_MIE | MSTATUS_MPIE)) | (mie << 7) | MSTATUS_MPP;
    s->mepc->write(s->pc);
    s->mcause->write(0x80000000u | r.intr_cause);
    s->mstatus->write(mstatus);
    s->pc = r.pc_rdata;
  }

  s->log_reg_write.clear();
  s->log_mem_read.clear();
  s->log_mem_write.clear();
  cur_ = &r;
  mem_ops_ = 0;
  mem_failed_ = false;
  reg_t instret_before = s->minstret->read();
  proc_->step(1);
  cur_ = nullptr;
  if (mem_failed_ || !error_.empty()) return false;
  bool spike_trapped = s->minstret->read() == instret_before;

  // Instruction and trap
  if (!r.trap) {
    uint32_t spike_insn = 0;
    mmio_fetch(r.pc_rdata, 4, (uint8_t*)&spike_insn);
    if ((r.insn & 3) == 3 && spike_insn != r.insn)
      return fail(where(r) + "instruction: Spike fetched " + hex(spike_insn));
  }
  if (spike_trapped != r.trap)
    return fail(where(r) + (r.trap ? "RTL trapped (cause " + std::to_string(r.exc_cause) + "), Spike did not"
                                   : "Spike trapped (mcause " + hex(s->mcause->read()) + "), RTL did not"));
  if (r.trap && ((uint32_t)s->mcause->read() != r.exc_cause || (uint32_t)s->mepc->read() != r.pc_rdata))
    return fail(where(r) + "trap: RTL cause " + std::to_string(r.exc_cause) + ", Spike mcause " +
                hex(s->mcause->read()) + " mepc " + hex(s->mepc->read()));
  if ((uint32_t)s->pc != r.pc_wdata)
    return fail(where(r) + "next PC: RTL " + hex(r.pc_wdata) + " Spike " + hex(s->pc));

  // Register writeback
  uint32_t csr;
  if (!r.trap && is_csr_read(r.insn, &csr) && csr_synced(csr)) {
    s->XPR.write(r.rd_addr, (int32_t)r.rd_wdata);
  } else {
    uint32_t spike_rd = 0, spike_val = 0;
    for (auto& [key, val] : s->log_reg_write) {
      if ((key & 0xf) != 0) continue;  // integer registers only
      spike_rd = (uint32_t)(key >> 4);
      spike_val = (uint32_t)val.v[0];
    }
    if (spike_rd == 0) spike_val = 0;
    uint32_t rtl_rd = r.rd_addr, rtl_val = rtl_rd ? r.rd_wdata : 0;
    if (spike_rd != rtl_rd || spike_val != rtl_val)
      return fail(where(r) + "writeback: RTL x" + std::to_string(rtl_rd) + "=" + hex(rtl_val) +
                  " Spike x" + std::to_string(spike_rd) + "=" + hex(spike_val));
  }
  if ((r.mem_rmask || r.mem_wmask) && mem_ops_ == 0 && !r.trap)
    return fail(where(r) + "RTL accessed memory at " + hex(r.mem_addr) + ", Spike did not");

  retired_++;
  return true;
}

}  // namespace torus
