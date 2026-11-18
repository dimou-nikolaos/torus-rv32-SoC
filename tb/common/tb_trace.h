// FST waveform of a testbench, written when the test runs with --trace=FILE.
// Tracing is compiled in only with -DTORUS_TRACE=ON (cmake/Verilated.cmake);
// other builds reject --trace instead of silently writing nothing.
#pragma once

#include <verilated.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#if VM_TRACE
#include <verilated_fst_c.h>
#endif

namespace torus {

class TbTrace {
 public:
  template <class Dut>
  void open(VerilatedContext* ctx, Dut* dut, const std::string& file) {
    if (file.empty()) return;
#if VM_TRACE
    ctx->traceEverOn(true);
    fst_ = std::make_unique<VerilatedFstC>();
    dut->trace(fst_.get(), 99);
    fst_->open(file.c_str());
#else
    (void)ctx;
    (void)dut;
    std::fprintf(stderr, "--trace: built without waveforms; reconfigure with -DTORUS_TRACE=ON\n");
    std::exit(2);
#endif
  }
  void dump(uint64_t t) {
#if VM_TRACE
    if (fst_) fst_->dump(t);
#else
    (void)t;
#endif
  }
  void close() {
#if VM_TRACE
    if (fst_) fst_->close();
#endif
  }

 private:
#if VM_TRACE
  std::unique_ptr<VerilatedFstC> fst_;
#endif
};

}  // namespace torus
