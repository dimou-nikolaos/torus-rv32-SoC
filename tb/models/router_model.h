// Cycle-accurate model of rtl/noc/noc_router.sv and of the credit link pieces
// around it. State changes only in edge(), with the RTL's clock-edge semantics;
// everything else is the combinational view of the current cycle.
#pragma once
#include <array>
#include <cstdint>
#include <deque>

#include "noc_flit.h"

namespace torus {

constexpr int kPorts = 3;
constexpr int kL = 0, kX = 1, kY = 2;

struct LinkFwd {
  bool valid = false;
  Port la = Port::Local;
  Flit flit;
};

struct Entry {
  Port la;
  Flit flit;
};

inline int credit_rtt(int slices) { return 4 + 2 * slices; }

inline uint8_t ring_next(uint8_t c, int n, int dir) {
  return dir == 0 ? (uint8_t)((c + 1) % n) : (uint8_t)((c + n - 1) % n);
}

inline Port route(int x, int y, int dx, int dy) {
  if (x != dx) return Port::X;
  if (y != dy) return Port::Y;
  return Port::Local;
}

// Fixed-latency delay line: what goes in at an edge comes out `n` edges later
// (n = 0 is a wire, handled by the caller).
template <typename T>
class Delay {
 public:
  explicit Delay(int n = 0) : q_(n, T{}) {}
  int depth() const { return (int)q_.size(); }
  const T& out() const { return q_.back(); }
  void edge(const T& in) {
    if (q_.empty()) return;
    q_.pop_back();
    q_.push_front(in);
  }

 private:
  std::deque<T> q_;
};

struct RouterEvents {
  std::array<bool, kPorts> flit{}, credit_stall{}, bubble_hold{}, bubble{}, arb_loss{};
};

class RouterModel {
 public:
  RouterModel(int dir, int gx, int gy, int depth, int x, int y)
      : dir_(dir), gx_(gx), gy_(gy), depth_(depth), x_(x), y_(y) {
    credits_.fill(depth);
    rr_.fill(0);
  }

  // ---- combinational view
  const LinkFwd& out(int o) const { return out_[o]; }
  bool credit_out(int i) const { return credit_q_[i]; }
  int credits(int o) const { return credits_[o]; }
  int occupancy(int i) const { return (int)fifo_[i].size(); }
  int x() const { return x_; }
  int y() const { return y_; }

  static bool entering(int i, int o) { return (o == kX && i == kL) || (o == kY && (i == kL || i == kX)); }

  // Grant per output (input index or -1) for the current state, and events.
  std::array<int, kPorts> allocate(RouterEvents* ev = nullptr) const {
    std::array<int, kPorts> g;
    g.fill(-1);
    for (int o = 0; o < kPorts; o++) {
      bool cand_any = false, elig_any = false, held = false;
      std::array<bool, kPorts> elig{};
      for (int i = 0; i < kPorts; i++) {
        bool cand = !fifo_[i].empty() && (int)fifo_[i].front().la == o;
        cand_any |= cand;
        elig[i] = cand && (entering(i, o) ? credits_[o] >= 2 : credits_[o] >= 1);
        held |= cand && entering(i, o) && credits_[o] == 1;
        elig_any |= elig[i];
      }
      for (int k = 1; k <= kPorts && g[o] < 0; k++) {
        int i = (rr_[o] + k) % kPorts;
        if (elig[i]) g[o] = i;
      }
      if (ev) {
        ev->flit[o] = g[o] >= 0;
        ev->credit_stall[o] = cand_any && credits_[o] == 0;
        ev->bubble_hold[o] = held;
        ev->bubble[o] = elig_any && g[o] < 0;
        for (int i = 0; i < kPorts; i++)
          if (elig[i] && g[o] != i) ev->arb_loss[i] = true;
      }
    }
    return g;
  }

  // ---- clock edge
  void edge(const std::array<LinkFwd, kPorts>& in, const std::array<bool, kPorts>& credit_in) {
    auto g = allocate();
    std::array<bool, kPorts> pop{};
    for (int o = 0; o < kPorts; o++) {
      bool send = g[o] >= 0;
      credits_[o] += (credit_in[o] ? 1 : 0) - (send ? 1 : 0);
      out_[o].valid = send;
      if (send) {
        const Entry& e = fifo_[g[o]].front();
        int nx = x_, ny = y_;
        if (o == kX) nx = ring_next((uint8_t)x_, gx_, dir_);
        if (o == kY) ny = ring_next((uint8_t)y_, gy_, dir_);
        out_[o].flit = e.flit;
        out_[o].la = route(nx, ny, e.flit.dst_x, e.flit.dst_y);
        pop[g[o]] = true;
        rr_[o] = g[o];
      }
    }
    for (int i = 0; i < kPorts; i++) {
      if (pop[i]) fifo_[i].pop_front();
      if (in[i].valid) fifo_[i].push_back({in[i].la, in[i].flit});
      credit_q_[i] = pop[i];
    }
  }

 private:
  int dir_, gx_, gy_, depth_, x_, y_;
  std::array<std::deque<Entry>, kPorts> fifo_;
  std::array<int, kPorts> credits_, rr_;
  std::array<LinkFwd, kPorts> out_{};
  std::array<bool, kPorts> credit_q_{};
};

}  // namespace torus
