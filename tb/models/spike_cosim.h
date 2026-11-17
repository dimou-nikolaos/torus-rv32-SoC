// Spike lockstep checker for one hart (SPEC.md, Verification strategy).
//
// Spike is stepped once per instruction the RTL retires (RVFI) and must agree on
// PC, instruction, trap and register writeback. Spike gets no direct memory:
// every fetch, load and store goes through this class, which
//   - serves fetches from a mirror of the scratchpad (ELF image + stores),
//   - checks each load's address against RVFI and returns the RTL's value
//     (the sync rule for data Spike cannot predict: MMIO, DMA, remote writes),
//   - checks each store's address, size and data against RVFI.
// Counter and implementation-defined CSR reads are synced from the RTL too.
//
// CV32E40X reports data bus errors as an NMI (mcause 1024 load, 1025 store)
// after the faulting instruction retires; Spike has no such NMI, so the checker
// performs the trap entry itself when RVFI reports it, and only then.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "riscv/cfg.h"
#include "riscv/simif.h"

class processor_t;

namespace torus {

struct RvfiRecord {
  uint64_t order;
  uint32_t pc_rdata, pc_wdata, insn;
  bool trap;
  uint32_t exc_cause;
  bool intr;            // first instruction of a trap handler entered by interrupt/NMI
  uint32_t intr_cause;
  uint32_t rd_addr, rd_wdata;
  uint32_t mem_addr, mem_rmask, mem_wmask, mem_rdata, mem_wdata;
  bool mem_err;
};

class SpikeCosim : public simif_t {
 public:
  // isa: e.g. "rv32im_zicsr_zifencei". spm_bytes: scratchpad mirror size.
  // check_spm_loads: also compare scratchpad loads with the mirror. Only valid
  // when the core is the only writer and never reads bytes it did not write
  // first (the scratchpad powers up random); the SoC testbench leaves it off.
  SpikeCosim(const std::string& isa, uint32_t start_pc, uint32_t hartid, uint32_t spm_bytes,
             bool check_spm_loads);
  ~SpikeCosim() override;

  void write_mem(uint32_t addr, const uint8_t* data, size_t len);

  // Steps Spike for one retired instruction. Returns false and fills error()
  // on the first mismatch.
  bool step(const RvfiRecord& r);
  const std::string& error() const { return error_; }
  uint64_t retired() const { return retired_; }

  // simif_t
  char* addr_to_mem(reg_t) override { return nullptr; }
  bool mmio_fetch(reg_t paddr, size_t len, uint8_t* bytes) override;
  bool mmio_load(reg_t paddr, size_t len, uint8_t* bytes) override;
  bool mmio_store(reg_t paddr, size_t len, const uint8_t* bytes) override;
  void proc_reset(unsigned) override {}
  const cfg_t& get_cfg() const override { return cfg_; }
  const std::map<size_t, processor_t*>& get_harts() const override { return harts_; }
  const char* get_symbol(uint64_t) override { return nullptr; }

 private:
  bool fail(const std::string& msg);
  std::string where(const RvfiRecord& r) const;

  cfg_t cfg_;
  std::unique_ptr<processor_t> proc_;
  std::map<size_t, processor_t*> harts_;
  std::vector<uint8_t> spm_;
  bool check_spm_loads_;
  const RvfiRecord* cur_ = nullptr;
  int mem_ops_ = 0;
  bool mem_failed_ = false;
  bool nmi_pending_ = false;
  std::string error_;
  uint64_t retired_ = 0;
  FILE* null_log_ = nullptr;
  std::ostringstream sout_;
};

}  // namespace torus
