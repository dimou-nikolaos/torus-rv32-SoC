// The simulated SoC as a libtorus Transport: the Verilated tb_soc_top, its two
// clocks (fabric and AXI, asynchronous), the AXI bus functional model on the
// I/O tile's slave port, and per tile the console, the exit code and the Spike
// lockstep fed from RVFI. One SimHost per process (the DPI callbacks find it).
//
// Options (TbArgs): --seed, --trace=f.fst, --cycles=MAX (fabric cycles before a
// timeout), --axi-half=N and --fabric-half=N (clock half periods, default 4
// and 5), --no-cosim, --isa=..., --rvfi-trace.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tb_args.h"
#include "torus.h"

namespace torus {

struct SimTile {
  bool exited = false;      // console exit seen
  int exit_code = 0;
  uint64_t rvfi_count = 0;  // retirements reported while running
  bool sw_cleared = false;  // the program cleared its own counters
  bool cosim = false;
  bool cosim_failed = false;
  uint64_t cosim_retired = 0;
};

class SimHost : public Transport {
 public:
  SimHost(int argc, char** argv, const TbArgs& args);
  ~SimHost() override;

  static int nx();
  static int ny();

  // Transport (blocking in simulated time; throws on SLVERR or a timeout)
  void write(uint32_t addr, const uint8_t* data, const uint16_t* strb, unsigned beats) override;
  void read(uint32_t addr, uint8_t* data, unsigned beats) override;
  void sync() override;

  void run(uint64_t fabric_cycles);
  uint64_t cycles() const;              // fabric cycles since reset
  uint64_t axi_cycles() const;

  // Spike lockstep for compute tile n (y * NX + x), started from this image
  void attach_cosim(int n, const ElfImage& img);
  const SimTile& tile(int n) const;
  bool all_exited() const;
  // An RTL assertion fired or a lockstep check failed
  bool failed() const;
  void finish();

  struct Impl;   // public for the DPI callbacks only

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace torus
