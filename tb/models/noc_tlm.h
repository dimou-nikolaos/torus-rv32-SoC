// Transaction-level model of one NoC plane (SPEC.md, Verification strategy):
// deterministic X-first paths on one-way rings. For every flit it gives the
// router outputs it crosses (so per-link flit counts can be compared exactly
// with the RTL), its hop count and its zero-load latency.
#pragma once
#include <map>
#include <vector>

#include "router_model.h"

namespace torus {

class NocTlm {
 public:
  NocTlm(int gx, int gy, int dir, int slice) : gx_(gx), gy_(gy), dir_(dir), slice_(slice) {}

  int node(int x, int y) const { return y * gx_ + x; }
  int dist(int from, int to, int n) const { return dir_ == 0 ? (to - from + n) % n : (from - to + n) % n; }
  int hops(int sx, int sy, int dx, int dy) const { return dist(sx, dx, gx_) + dist(sy, dy, gy_); }

  // Router outputs (node, port) the flit leaves through, ejection included
  std::vector<std::pair<int, int>> path(int sx, int sy, int dx, int dy) const {
    std::vector<std::pair<int, int>> p;
    int x = sx, y = sy;
    while (x != dx) { p.push_back({node(x, y), kX}); x = ring_next((uint8_t)x, gx_, dir_); }
    while (y != dy) { p.push_back({node(x, y), kY}); y = ring_next((uint8_t)y, gy_, dir_); }
    p.push_back({node(x, y), kL});
    return p;
  }

  // Cycles from the endpoint's send decision to the flit at the head of the
  // destination's ejection buffer, with no other traffic: sender register,
  // source router (2), each ring hop (slices + 2), ejection buffer write.
  int zero_load_latency(int sx, int sy, int dx, int dy) const {
    return 4 + hops(sx, sy, dx, dy) * (2 + slice_);
  }

  void deliver(int sx, int sy, int dx, int dy) {
    for (auto& l : path(sx, sy, dx, dy)) link_count_[l]++;
  }
  uint64_t link_count(int node, int port) const {
    auto it = link_count_.find({node, port});
    return it == link_count_.end() ? 0 : it->second;
  }

 private:
  int gx_, gy_, dir_, slice_;
  std::map<std::pair<int, int>, uint64_t> link_count_;
};

}  // namespace torus
