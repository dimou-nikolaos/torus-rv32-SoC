// Golden models for the scratchpad: the bank arbiter, cycle-accurate, and the
// memory contents. See SPEC.md, Scratchpad bank arbiter.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace torus {

constexpr int kSpmPorts = 7;
constexpr int kSpmBanks = 4;
constexpr int kSpmBytes = 64 * 1024;
constexpr int kNiuLo = 1, kNiuHi = 4;  // round-robin group (RX0, RX1, TX-DMA, TX-reply)

inline int spm_bank_of(uint32_t addr) { return (addr >> 4) & 3; }

// One bank's arbiter. step() takes this cycle's requests and returns the grant
// mask, then advances state as the RTL does on the clock edge.
class ArbiterModel {
 public:
  explicit ArbiterModel(int starve_limit) : limit_(starve_limit) { reset(); }

  void reset() {
    age_.fill(0);
    lock_ = false;
    lock_port_ = 0;
    rr_prom_ = kSpmPorts - 1;
    rr_grp_ = kNiuHi;
  }

  // Worst-case wait (denied cycles) the RTL asserts against.
  int bound() const { return limit_ + 2 * (kSpmPorts - 1) + 1; }

  uint32_t step(uint32_t req, uint32_t lock, uint32_t* promoted_out) {
    uint32_t gnt = 0, prom = 0;
    bool use_prom = false, lock_path = false;
    for (int i = 0; i < kSpmPorts; i++)
      if ((req >> i & 1) && age_[i] >= limit_) prom |= 1u << i;

    if (lock_ && (req >> lock_port_ & 1)) {
      gnt = 1u << lock_port_;
      lock_path = true;
    } else if (prom) {
      use_prom = true;
      gnt = rr_pick(prom, 0, kSpmPorts - 1, rr_prom_);
    } else if (req & 1) {
      gnt = 1;  // engine
    } else if (req & group_mask()) {
      gnt = rr_pick(req & group_mask(), kNiuLo, kNiuHi, rr_grp_);
    } else if (req >> 5 & 1) {
      gnt = 1u << 5;  // core data
    } else if (req >> 6 & 1) {
      gnt = 1u << 6;  // core instruction
    }
    *promoted_out = use_prom ? gnt : 0;

    // State update (clock edge)
    int g = gnt ? __builtin_ctz(gnt) : 0;
    bool new_lock = (gnt & lock) && !lock_;
    lock_ = new_lock;
    lock_port_ = g;
    if (use_prom && gnt) rr_prom_ = g;
    if (!use_prom && !lock_path && (gnt & group_mask())) rr_grp_ = g;
    for (int i = 0; i < kSpmPorts; i++) {
      if ((req >> i & 1) && !(gnt >> i & 1)) {
        if (age_[i] != limit_) age_[i]++;
      } else {
        age_[i] = 0;
      }
    }
    return gnt;
  }

 private:
  static uint32_t group_mask() { return ((1u << (kNiuHi + 1)) - 1) & ~((1u << kNiuLo) - 1); }

  static uint32_t rr_pick(uint32_t cand, int lo, int hi, int last) {
    int n = hi - lo + 1;
    for (int k = 1; k <= n; k++) {
      int idx = lo + ((last - lo + k) % n + n) % n;
      if (cand >> idx & 1) return 1u << idx;
    }
    return 0;
  }

  int limit_;
  std::array<int, kSpmPorts> age_{};
  bool lock_ = false;
  int lock_port_ = 0;
  int rr_prom_ = 0, rr_grp_ = 0;
};

// Byte-addressed scratchpad contents.
class SpmMemory {
 public:
  SpmMemory() : mem_(kSpmBytes, 0) {}
  void write128(uint32_t addr, const uint8_t* data, uint16_t be) {
    uint32_t base = addr & (kSpmBytes - 1) & ~15u;
    for (int b = 0; b < 16; b++)
      if (be >> b & 1) mem_[base + b] = data[b];
  }
  void read128(uint32_t addr, uint8_t* out) const {
    memcpy(out, &mem_[addr & (kSpmBytes - 1) & ~15u], 16);
  }
 private:
  std::vector<uint8_t> mem_;
};

}  // namespace torus
