// AXI4 master bus functional model (SPEC.md, I/O tile: the host side in
// Verilator). Drives the s_axi_* pins of a Verilated top directly, one AXI
// clock cycle at a time:
//
//   bfm.drive();   // clock low: set valids, payloads and readies
//   dut->eval();
//   bfm.sample();  // still low: handshakes happen where valid && ready
//   ...raise the AXI clock...
//
// Operations are queued and complete in the background: writes are INCR bursts
// of 16-byte beats with per-beat strobes (one AW at a time ahead of its W
// beats, the next AW may go out early), reads are INCR bursts with up to
// max_reads outstanding, answered in order. Random gaps before each valid and
// random backpressure on B and R exercise the slave's handshakes.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace torus {

struct Beat {
  uint32_t w[4] = {};
};

struct AxiResult {
  bool done = false;
  uint32_t resp = 0;          // worst response seen (0 OKAY, 2 SLVERR)
  std::vector<Beat> data;     // reads
};

template <class Dut>
class AxiMaster {
 public:
  explicit AxiMaster(Dut* dut, uint64_t seed = 1) : dut_(dut), rng_(seed) { idle(); }

  // Knobs: percent chance per cycle to hold a valid back, or a ready low
  int gap_pct = 0, bp_pct = 0;
  unsigned max_reads = 8;
  uint32_t size = 4, burst = 1;  // AxSIZE and AxBURST (tests may break them)

  int write(uint32_t addr, const std::vector<Beat>& beats, const std::vector<uint16_t>& strb, unsigned id = 0) {
    int h = next_handle_++;
    results_[h];
    wq_.push_back({h, addr, beats, strb, id, size, burst});
    return h;
  }
  int read(uint32_t addr, unsigned beats, unsigned id = 0) {
    int h = next_handle_++;
    results_[h];
    arq_.push_back({h, addr, beats, id, size, burst});
    return h;
  }
  bool done(int h) const { return results_.at(h).done; }
  const AxiResult& result(int h) const { return results_.at(h); }
  void forget(int h) { results_.erase(h); }
  bool idle_all() const { return wq_.empty() && wbeats_.empty() && bq_.empty() && arq_.empty() && rq_.empty(); }
  unsigned reads_in_flight() const { return (unsigned)rq_.size(); }
  uint64_t r_beats = 0, w_beats = 0;  // handshakes, for throughput

  void drive() {
    // AW: the next queued write
    if (!aw_on_ && !wq_.empty() && !gap()) aw_on_ = true;
    if (aw_on_) {
      const Wr& w = wq_.front();
      dut_->s_axi_awvalid_i = 1;
      dut_->s_axi_awid_i = w.id;
      dut_->s_axi_awaddr_i = w.addr;
      dut_->s_axi_awlen_i = (uint32_t)w.beats.size() - 1;
      dut_->s_axi_awsize_i = w.size;
      dut_->s_axi_awburst_i = w.burst;
    } else {
      dut_->s_axi_awvalid_i = 0;
    }
    // W: beats of writes whose AW went out, in order
    if (!w_on_ && !wbeats_.empty() && !gap()) w_on_ = true;
    if (w_on_) {
      const WBeat& b = wbeats_.front();
      dut_->s_axi_wvalid_i = 1;
      for (int i = 0; i < 4; i++) dut_->s_axi_wdata_i[i] = b.beat.w[i];
      dut_->s_axi_wstrb_i = b.strb;
      dut_->s_axi_wlast_i = b.last;
    } else {
      dut_->s_axi_wvalid_i = 0;
    }
    // AR
    if (!ar_on_ && !arq_.empty() && rq_.size() < max_reads && !gap()) ar_on_ = true;
    if (ar_on_) {
      const Rd& r = arq_.front();
      dut_->s_axi_arvalid_i = 1;
      dut_->s_axi_arid_i = r.id;
      dut_->s_axi_araddr_i = r.addr;
      dut_->s_axi_arlen_i = r.beats - 1;
      dut_->s_axi_arsize_i = r.size;
      dut_->s_axi_arburst_i = r.burst;
    } else {
      dut_->s_axi_arvalid_i = 0;
    }
    dut_->s_axi_bready_i = !bp();
    dut_->s_axi_rready_i = !bp();
  }

  // Returns false on a protocol violation by the slave (message in error())
  bool sample() {
    if (aw_on_ && dut_->s_axi_awready_o) {
      Wr w = wq_.front();
      wq_.pop_front();
      for (size_t i = 0; i < w.beats.size(); i++)
        wbeats_.push_back({w.beats[i], w.strb[i], i + 1 == w.beats.size()});
      bq_.push_back({w.handle, w.id});
      aw_on_ = false;
    }
    if (w_on_ && dut_->s_axi_wready_o) {
      wbeats_.pop_front();
      w_on_ = false;
      w_beats++;
    }
    if (ar_on_ && dut_->s_axi_arready_o) {
      Rd r = arq_.front();
      arq_.pop_front();
      rq_.push_back({r.handle, r.id, r.beats});
      ar_on_ = false;
    }
    if (dut_->s_axi_bvalid_o && dut_->s_axi_bready_i) {
      if (bq_.empty()) return fail("B with no write outstanding");
      auto [h, id] = bq_.front();
      if (dut_->s_axi_bid_o != id) return fail("B id mismatch");
      bq_.pop_front();
      results_[h].resp = dut_->s_axi_bresp_o;
      results_[h].done = true;
    }
    if (dut_->s_axi_rvalid_o && dut_->s_axi_rready_i) {
      if (rq_.empty()) return fail("R with no read outstanding");
      RdOut& r = rq_.front();
      if (dut_->s_axi_rid_o != r.id) return fail("R id mismatch");
      AxiResult& res = results_[r.handle];
      Beat b;
      for (int i = 0; i < 4; i++) b.w[i] = dut_->s_axi_rdata_o[i];
      res.data.push_back(b);
      res.resp = std::max<uint32_t>(res.resp, dut_->s_axi_rresp_o);
      r_beats++;
      bool last = res.data.size() == r.beats;
      if ((bool)dut_->s_axi_rlast_o != last) return fail("RLAST on the wrong beat");
      if (last) {
        res.done = true;
        rq_.pop_front();
      }
    }
    return true;
  }

  const std::string& error() const { return err_; }

 private:
  struct Wr {
    int handle;
    uint32_t addr;
    std::vector<Beat> beats;
    std::vector<uint16_t> strb;
    unsigned id;
    uint32_t size, burst;
  };
  struct WBeat {
    Beat beat;
    uint16_t strb;
    bool last;
  };
  struct Rd {
    int handle;
    uint32_t addr;
    unsigned beats, id;
    uint32_t size, burst;
  };
  struct RdOut {
    int handle;
    unsigned id, beats;
  };

  bool gap() { return gap_pct && (int)(rng_() % 100) < gap_pct; }
  bool bp() { return bp_pct && (int)(rng_() % 100) < bp_pct; }
  bool fail(const char* m) {
    err_ = m;
    return false;
  }
  void idle() {
    dut_->s_axi_awvalid_i = 0;
    dut_->s_axi_wvalid_i = 0;
    dut_->s_axi_arvalid_i = 0;
    dut_->s_axi_bready_i = 0;
    dut_->s_axi_rready_i = 0;
  }

  Dut* dut_;
  std::mt19937_64 rng_;
  std::deque<Wr> wq_;
  std::deque<WBeat> wbeats_;
  std::deque<std::pair<int, unsigned>> bq_;
  std::deque<Rd> arq_;
  std::deque<RdOut> rq_;
  std::map<int, AxiResult> results_;
  bool aw_on_ = false, w_on_ = false, ar_on_ = false;
  int next_handle_ = 1;
  std::string err_;
};

}  // namespace torus
