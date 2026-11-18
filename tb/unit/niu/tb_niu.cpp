// NIU unit test (SPEC.md, Verification strategy: NIU and DMA).
//
// The NIU sits on its real scratchpad. The testbench drives the registers, plays
// the network of both planes on both sides, models tile control and the
// counter bank on the remote register port, and adds random core reads for
// bank conflicts. A DMA model predicts every injected flit, and the plane it
// leaves on, from a memory model; RX flits are predicted per plane as an
// ordered list of records (scratchpad write, register writes, READ_REQ into the
// queue, atomic, dropped error) that the NIU must perform in exactly that order
// on that plane's RX path, whatever the bank contention; every accepted
// READ_REQ predicts its reply flits. The two RX paths retire independently, so
// each plane writes its own half of the written regions (and READ_REQs on a
// plane read only data that plane writes): the model stays exact. Checks:
//   - TX: per channel, flits in address order with the right header, data,
//     tag, lookahead port and plane (automatic, or forced by CMD), for WRITE,
//     READ_REQ and ATOMIC_INC; STATUS busy/done/error, a read completing only
//     when its last reply landed; every command error;
//   - RX, on both planes: byte enables, register writes per lane, in-order
//     retire across targets, atomics as locked adds, the doorbell, errors
//     dropped;
//   - TX-reply: replies in order, on the plane towards the requester, with the
//     data the source held when the READ_REQ retired (rule 2), from the
//     scratchpad, registers or zeros;
//   - READ_REQ storm: with injection blocked, a full queue of READ_REQs and
//     more writes on both planes retire without RX ever stalling;
//   - memory at the end equals the model;
//   - full rate: one flit per cycle injected per plane, ejected per plane,
//     replied, and DMA and replies sharing a plane; all four streams at once
//     with DMA and replies on different planes.
#include <svdpi.h>
#include <verilated.h>
#include <verilated_fst_c.h>

#include <deque>
#include <memory>
#include <random>
#include <vector>

#include "Vtb_niu_top.h"
#include "Vtb_niu_top__Dpi.h"
#include "noc_flit.h"
#include "noc_tlm.h"
#include "router_model.h"
#include "tb_args.h"

using namespace torus;

namespace {

constexpr int kGX = 4, kGY = 4, kDepth = 5, kRQ = 6;  // row kGY-1 is the I/O row
constexpr int kMeX = 1, kMeY = 2;
// Region A: DMA and reply sources, never written. B1: RX writes and reply
// sources. B2: RX writes and the landing area of our own reads. Plane p writes
// only the p-th half of B1 and of B2 (8 KB each).
constexpr uint32_t kSpm = 64 * 1024, kRegionB = 0x8000, kRegionB2 = 0xC000, kHalf = 0x2000;
constexpr uint32_t kTctrl = 0x10001000, kPerf = 0x10002000;
uint32_t b1(int p) { return kRegionB + kHalf * (uint32_t)p; }
uint32_t b2(int p) { return kRegionB2 + kHalf * (uint32_t)p; }

svScope bank_scope(int b) {
  std::string n = "TOP.tb_niu_top.u_spm.g_bank[" + std::to_string(b) + "].u_bank";
  svScope s = svGetScopeFromName(n.c_str());
  if (!s) { fprintf(stderr, "no scope %s\n", n.c_str()); exit(2); }
  return s;
}
void bd_write32(uint32_t a, uint32_t d) {
  svSetScope(bank_scope((a >> 4) & 3));
  spm_bank_bd_write((int)(a >> 6), (int)((a >> 2) & 3), (int)d);
}
uint32_t bd_read32(uint32_t a) {
  svSetScope(bank_scope((a >> 4) & 3));
  return (uint32_t)spm_bank_bd_read((int)(a >> 6), (int)((a >> 2) & 3));
}

// Remote registers as the testbench models them: a fixed value per address,
// and an error for about one address in eight (the NIU answers with zeros)
uint32_t reg_val(uint32_t a) { return a * 0x9e3779b1u ^ 0x5a5a0000u; }
bool reg_err(uint32_t a) { return ((a >> 2) * 2654435761u) >> 29 == 0; }

// Plane of a flit between two tiles of this grid
int plane(int sx, int sy, int dx, int dy) { return select_plane(kGX, kGY, sx, sy, dx, dy); }

// Flits in the packed per-plane ports
template <typename W>
void put_flit(W& vec, int p, const Flit& f) {
  uint32_t w[Flit::kWords];
  f.pack(w);
  for (int b = 0; b < Flit::kBits; b++) {
    int bit = p * Flit::kBits + b;
    if (w[b / 32] >> (b % 32) & 1) vec[bit / 32] |= 1u << (bit % 32);
    else vec[bit / 32] &= ~(1u << (bit % 32));
  }
}
template <typename W>
Flit get_flit(const W& vec, int p) {
  uint32_t w[Flit::kWords] = {};
  for (int b = 0; b < Flit::kBits; b++) {
    int bit = p * Flit::kBits + b;
    if (vec[bit / 32] >> (bit % 32) & 1) w[b / 32] |= 1u << (b % 32);
  }
  return Flit::unpack(w);
}

struct Cmd {
  uint32_t src, dst_addr, xy, len, cmd;
};

struct TxExp {
  Flit f;
  int plane;
};

enum class Act { SpmWrite, RegWrite, ReadReq, Atomic, Drop };
struct RegWr {
  uint32_t addr, wdata, be;
};
struct RxRec {
  Act kind;
  uint32_t addr = 0;            // SpmWrite, Atomic: 16-byte word
  std::deque<RegWr> regs;       // RegWrite: one per lane, in order
  bool rsp = false;             // a reply to our own READ_REQ
  bool doorbell = false;
  bool err = false;             // counted as an error at retire
  std::vector<std::pair<TxExp, int>> replies;  // ReadReq: its reply flits, queued when it retires
};

struct Range {
  uint32_t lo, hi;
  int id;
};

}  // namespace

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 200000);
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  ctx->randSeed((int)(args.seed & 0x7fffffff));
  auto dut = std::make_unique<Vtb_niu_top>(ctx.get());
  std::unique_ptr<VerilatedFstC> tfp;
  if (!args.trace.empty()) {
    ctx->traceEverOn(true);
    tfp = std::make_unique<VerilatedFstC>();
    dut->trace(tfp.get(), 99);
    tfp->open(args.trace.c_str());
  }
  auto half = [&](int clk) {
    dut->clk_i = clk;
    dut->eval();
    if (tfp) tfp->dump(ctx->time());
    ctx->timeInc(1);
  };
  dut->x_i = kMeX;
  dut->y_i = kMeY;
  ctx->assertOn(false);
  dut->rst_ni = 1;
  dut->eval();
  dut->rst_ni = 0;
  for (int i = 0; i < 3; i++) { half(0); half(1); }
  dut->rst_ni = 1;
  ctx->assertOn(true);

  // Memory: random contents, mirrored in the model
  std::vector<uint8_t> mem(kSpm);
  for (uint32_t a = 0; a < kSpm; a += 4) {
    uint32_t w = (uint32_t)rng();
    bd_write32(a, w);
    memcpy(&mem[a], &w, 4);
  }

  // ---------------------------------------------------------------- state
  std::deque<TxExp> exp_tx[2];      // expected DMA flits per channel, with their plane
  std::deque<std::pair<TxExp, int>> exp_rep;  // expected reply flits, with their request id
  std::vector<Range> pending;       // reply sources not fully sent yet: RX writes avoid them
  std::vector<int> rep_left;        // per request id: reply flits not yet seen
  uint8_t seq[2] = {0, 0};
  bool busy[2] = {false, false};
  uint16_t done[2] = {0, 0};
  bool err[2] = {false, false};
  int rsp_outstanding = 0;          // our READ_REQ: replies not yet retired
  uint8_t rsp_tag = 0;
  int rsp_plane = 0;                // the plane its replies arrive on
  std::deque<Flit> rsp_to_send;     // the network's answer to our READ_REQ
  std::deque<RxRec> exp_rx[2];      // per plane
  std::deque<Flit> directed[2];     // RX flits a phase wants sent, before random ones
  std::deque<int> inj_credit_delay[2];
  int ej_credits[2] = {kDepth, kDepth};
  bool inj_block = false;
  int rq_open = 0;                  // READ_REQs sent whose replies are not all out
  uint64_t t = 0, tx_flits[2] = {0, 0}, rep_flits[2] = {0, 0}, rx_done[2] = {0, 0}, cmds = 0, bad_cmds = 0,
           n_atomic = 0, n_readreq = 0, n_regwr = 0, n_drop = 0, n_forced = 0;

  // Register traffic: one op per cycle; reads complete one cycle later
  struct RegOp {
    bool we;
    uint32_t addr, wdata;
  };
  std::deque<RegOp> reg_q;
  uint32_t last_read = 0;
  bool read_ready = false;
  // STATUS is sampled in the cycle its read is driven: the expected state is
  // snapshot at the end of that cycle. Our view of a DMA completion lags the
  // RTL by a cycle (we see the registered flit); a read completing on its last
  // reply's retire is applied one cycle late to match.
  bool snap_busy[2] = {false, false}, snap_err[2] = {false, false};
  uint16_t snap_done[2] = {0, 0};
  bool rsp_complete_next = false;
  bool rmt_rd_pending = false;
  uint32_t rmt_rd_addr = 0;

  // Network behaviour knobs per phase
  int inj_credit_lat = 2, ej_send_pct = 50, contention_pct = 30, inj_credit_jitter = 4;
  bool rx_random = true;

  auto overlaps_pending = [&](uint32_t lo, uint32_t hi) {
    for (auto& r : pending) if (lo < r.hi && r.lo < hi) return true;
    return false;
  };

  // Expected flits of a command, from the model memory
  auto expect_cmd = [&](int ch, const Cmd& c) {
    uint32_t op = c.cmd & 7, pf = (c.cmd >> 3) & 3;
    int dx = (int)(c.xy & 0xff), dy = (int)((c.xy >> 8) & 0xff);
    int pl = pf == 1 ? 0 : pf == 2 ? 1 : plane(kMeX, kMeY, dx, dy);
    uint32_t n = op == 0 ? c.len / 16 : 1;
    for (uint32_t i = 0; i < n; i++) {
      Flit f;
      f.dst_x = (uint8_t)dx;
      f.dst_y = (uint8_t)dy;
      f.src_x = kMeX;
      f.src_y = kMeY;
      f.op = (Op)op;
      f.addr = c.dst_addr + 16 * i;
      f.be = 0xffff;
      f.tag = (uint8_t)(ch << 7 | seq[ch]);
      seq[ch] = (seq[ch] + 1) & 0x7f;
      if (op == 0) {
        memcpy(f.data, &mem[c.src + 16 * i], 16);
      } else if (op == 1) {
        f.data[0] = c.src;
        f.data[1] = c.len;
        rsp_outstanding = (int)(c.len / 16);
        rsp_tag = f.tag;
        rsp_plane = plane(dx, dy, kMeX, kMeY);
      } else {
        f.be = (uint16_t)(0xF << (4 * ((c.dst_addr >> 2) & 3)));
        f.data[0] = c.cmd >> 16;
        f.data[1] = (c.cmd >> 5) & 1;
      }
      exp_tx[ch].push_back({f, pl});
    }
  };

  // What the NIU must do with an RX flit, applied to the model in RX order
  auto classify = [&](const Flit& f) {
    RxRec r;
    bool spm = f.addr < kSpm, reg = (f.addr >> 12) == (kTctrl >> 12) || (f.addr >> 12) == (kPerf >> 12);
    switch (f.op) {
      case Op::Write:
      case Op::ReadRsp:
        if (f.op == Op::ReadRsp && !(rsp_outstanding > 0 && f.tag == rsp_tag && spm)) {
          r.kind = Act::Drop;
        } else if (spm) {
          r.kind = Act::SpmWrite;
          r.addr = f.addr & 0xfff0;
          r.rsp = f.op == Op::ReadRsp;
          const uint8_t* d = (const uint8_t*)f.data;
          for (int b = 0; b < 16; b++)
            if (f.be >> b & 1) mem[r.addr + b] = d[b];
        } else if (reg) {
          r.kind = Act::RegWrite;
          for (uint32_t l = 0; l < 4; l++)
            if (f.be >> (4 * l) & 0xF) r.regs.push_back({(f.addr & ~15u) | 4 * l, f.data[l], (f.be >> (4 * l)) & 0xFu});
          n_regwr++;
        } else {
          r.kind = Act::Drop;
        }
        break;
      case Op::ReadReq: {
        uint32_t ret = f.data[0], len = f.data[1];
        bool ok = len && !(len & 15) && len <= kSpm && !(f.addr & 15) && ret < kSpm && !(ret & 15);
        if (!ok) { r.kind = Act::Drop; break; }
        r.kind = Act::ReadReq;
        n_readreq++;
        bool src_spm = spm && (uint64_t)f.addr + len <= kSpm;
        bool src_reg = reg && (f.addr & 0xfff) + len <= 4096;
        r.err = !src_spm && !src_reg;
        int id = (int)rep_left.size();
        rep_left.push_back((int)(len / 16));
        rq_open++;
        if (src_spm) pending.push_back({f.addr, f.addr + len, id});
        for (uint32_t i = 0; i < len / 16; i++) {
          Flit rf;
          rf.dst_x = f.src_x;
          rf.dst_y = f.src_y;
          rf.src_x = kMeX;
          rf.src_y = kMeY;
          rf.op = Op::ReadRsp;
          rf.addr = ret + 16 * i;
          rf.be = 0xffff;
          rf.tag = f.tag;
          for (int w = 0; w < 4; w++) {
            uint32_t a = f.addr + 16 * i + 4 * w;
            if (src_spm) memcpy(&rf.data[w], &mem[a], 4);
            else if (src_reg) rf.data[w] = reg_err(a) ? 0 : reg_val(a);
            else rf.data[w] = 0;
          }
          r.replies.push_back({{rf, plane(kMeX, kMeY, f.src_x, f.src_y)}, id});
        }
        break;
      }
      case Op::AtomicInc:
        if (spm && !(f.addr & 3)) {
          r.kind = Act::Atomic;
          r.addr = f.addr & 0xfff0;
          r.doorbell = f.data[1] & 1;
          uint32_t w;
          memcpy(&w, &mem[f.addr], 4);
          w += f.data[0];
          memcpy(&mem[f.addr], &w, 4);
          n_atomic++;
        } else {
          r.kind = Act::Drop;
        }
        break;
      default:
        r.kind = Act::Drop;
    }
    if (r.kind == Act::Drop) {
      r.err = true;
      n_drop++;
    }
    return r;
  };

  // A random RX flit on plane p (the model is applied when it is sent)
  auto random_rx = [&](int p) {
    Flit f;
    f.src_x = (uint8_t)(rng() % kGX);
    f.src_y = (uint8_t)(rng() % (kGY - 1));
    f.dst_x = kMeX;
    f.dst_y = kMeY;
    f.tag = (uint8_t)rng();
    for (auto& d : f.data) d = (uint32_t)rng();
    f.be = (uint16_t)rng();
    int kind = (int)(rng() % 100);
    auto pick_b = [&](uint32_t len) -> int64_t {
      for (int i = 0; i < 8; i++) {
        uint32_t base = rng() & 1 ? b1(p) : b2(p);
        uint32_t a = base + ((uint32_t)rng() % (kHalf - len + 1) & ~15u);
        if (!overlaps_pending(a, a + len)) return a;
      }
      return -1;
    };
    if (kind < 55) {
      int64_t a = pick_b(16);
      if (a >= 0) {
        f.op = Op::Write;
        f.addr = (uint32_t)a + (uint32_t)(rng() & 15);
        return f;
      }
      kind = 60;
    }
    if (kind < 65) {  // register write, random lanes
      f.op = Op::Write;
      f.addr = kTctrl + ((uint32_t)rng() & 0xffc);
      return f;
    }
    if (kind < 77 && rq_open < kRQ) {  // READ_REQ
      f.op = Op::ReadReq;
      uint32_t len = 16 * (1 + (uint32_t)(rng() % 32));
      int src = (int)(rng() % 100);
      if (src < 40) f.addr = (uint32_t)rng() % (kRegionB - len + 1) & ~15u;                    // region A
      else if (src < 70) f.addr = b1(p) + ((uint32_t)rng() % (kHalf - len + 1) & ~15u);        // B1, this plane's half
      else if (src < 85) { len = 16 * (1 + (uint32_t)(rng() % 4)); f.addr = (rng() & 1 ? kTctrl : kPerf) + ((uint32_t)rng() % (4096 - len + 1) & ~15u); }
      else if (src < 93) f.addr = 0x20000000u | ((uint32_t)rng() & 0xfff0);                  // unmapped
      else f.addr = kSpm - 16;                                                              // runs past the end
      f.data[0] = (uint32_t)rng() % kSpm & ~15u;
      f.data[1] = len;
      if (rng() % 16 == 0) f.data[1] = len + 4;  // malformed: dropped
      return f;
    }
    if (kind < 90) {  // atomic, sometimes with the doorbell
      int64_t a = pick_b(16);
      if (a >= 0) {
        f.op = Op::AtomicInc;
        f.addr = (uint32_t)a + 4 * (uint32_t)(rng() % 4);
        f.data[1] = rng() % 4 == 0;
        return f;
      }
    }
    // Dropped: reserved op, unmapped write, misaligned atomic, unexpected reply
    switch (rng() % 4) {
      case 0: f.op = Op::Multicast; f.addr = b1(p); break;
      case 1: f.op = Op::Write; f.addr = 0x20000000u | ((uint32_t)rng() & 0xfff0); break;
      case 2: f.op = Op::AtomicInc; f.addr = b1(p) + 2; break;
      default: f.op = Op::ReadRsp; f.addr = b1(p); f.tag |= 0x80; break;  // channel 0 tags have bit 7 clear
    }
    return f;
  };

  auto cycle = [&]() -> int {
    if (rsp_complete_next) {
      busy[0] = false;
      done[0]++;
      rsp_complete_next = false;
    }
    // ---- drive inputs
    dut->reg_req_i = 0;
    bool reg_read = false;
    if (!reg_q.empty()) {
      reg_read = !reg_q.front().we;
      RegOp op = reg_q.front();
      reg_q.pop_front();
      dut->reg_req_i = 1;
      dut->reg_we_i = op.we;
      dut->reg_addr_i = op.addr;
      dut->reg_wdata_i = op.wdata;
    }
    dut->inj_credit_i = 0;
    dut->ej_valid_i = 0;
    for (int p = 0; p < 2; p++) {
      auto& cd = inj_credit_delay[p];
      if (!inj_block && !cd.empty() && cd.front() <= 0) {
        dut->inj_credit_i |= (uint8_t)(1u << p);
        cd.pop_front();
      }
      // Ejection: our replies, directed flits, then random traffic, within the NIU's credits
      if (ej_credits[p] > 0) {
        Flit f;
        bool send = false;
        if (p == rsp_plane && !rsp_to_send.empty() && rng() % 2 == 0) {
          f = rsp_to_send.front();
          rsp_to_send.pop_front();
          send = true;
        } else if (!directed[p].empty()) {
          f = directed[p].front();
          directed[p].pop_front();
          send = true;
        } else if (rx_random && (int)(rng() % 100) < ej_send_pct) {
          f = random_rx(p);
          send = true;
        }
        if (send) {
          exp_rx[p].push_back(classify(f));
          put_flit(dut->ej_flit_i, p, f);
          dut->ej_valid_i |= (uint8_t)(1u << p);
          ej_credits[p]--;
        }
      }
    }
    // Remote register answer to last cycle's read
    dut->rmt_rvalid_i = rmt_rd_pending;
    dut->rmt_rdata_i = rmt_rd_pending && !reg_err(rmt_rd_addr) ? reg_val(rmt_rd_addr) : 0;
    dut->rmt_err_i = rmt_rd_pending && reg_err(rmt_rd_addr);
    dut->cd_req_i = (int)(rng() % 100) < contention_pct;
    dut->cd_addr_i = (uint16_t)rng();
    dut->ci_req_i = (int)(rng() % 100) < contention_pct;
    dut->ci_addr_i = (uint16_t)rng();
    half(0);

    // ---- observe this cycle (combinational outputs)
    for (int p = 0; p < 2; p++) {
      if (!(dut->rx_wr_o >> p & 1)) continue;
      uint32_t wa = (dut->rx_wr_addr_o >> (16 * p)) & 0xffff;
      bool add = dut->rx_wr_add_o >> p & 1;
      TB_CHECK(!exp_rx[p].empty(), "t=%llu unexpected RX%d scratchpad write", (unsigned long long)t, p);
      const RxRec& a = exp_rx[p].front();
      bool is_atomic = a.kind == Act::Atomic;
      TB_CHECK((a.kind == Act::SpmWrite || is_atomic) && a.addr == (wa & 0xfff0u) && add == is_atomic,
               "t=%llu RX%d out of order: scratchpad write at %04x (add %d), expected kind %d addr %08x",
               (unsigned long long)t, p, wa, add, (int)a.kind, a.addr);
    }
    rmt_rd_pending = false;
    if (dut->rmt_req_o && dut->rmt_we_o) {
      int p = dut->rmt_port_o;
      TB_CHECK(!exp_rx[p].empty() && exp_rx[p].front().kind == Act::RegWrite && !exp_rx[p].front().regs.empty(),
               "t=%llu unexpected register write %08x from RX%d", (unsigned long long)t, dut->rmt_addr_o, p);
      RegWr e = exp_rx[p].front().regs.front();
      exp_rx[p].front().regs.pop_front();
      TB_CHECK(e.addr == dut->rmt_addr_o && e.wdata == dut->rmt_wdata_o && e.be == dut->rmt_be_o,
               "t=%llu RX%d register write %08x=%08x be %x, expected %08x=%08x be %x", (unsigned long long)t, p,
               dut->rmt_addr_o, dut->rmt_wdata_o, dut->rmt_be_o, e.addr, e.wdata, e.be);
    } else if (dut->rmt_req_o) {
      TB_CHECK(dut->rmt_be_o == 0xF && (dut->rmt_addr_o & 3) == 0, "t=%llu bad register read", (unsigned long long)t);
      rmt_rd_pending = true;
      rmt_rd_addr = dut->rmt_addr_o;
    }
    bool want_err = false, want_bell = false;
    for (int p = 0; p < 2; p++) {
      if (!(dut->ev_rx_o >> p & 1)) continue;
      TB_CHECK(!exp_rx[p].empty(), "t=%llu RX%d retired with nothing expected", (unsigned long long)t, p);
      const RxRec& a = exp_rx[p].front();
      TB_CHECK(a.regs.empty(), "t=%llu RX%d retired with %zu register writes missing", (unsigned long long)t, p,
               a.regs.size());
      want_err |= a.err;
      want_bell |= a.doorbell;
      // The READ_REQ queue takes READ_REQs in retire order, whichever plane
      for (auto& rep : a.replies) exp_rep.push_back(rep);
      if (a.rsp && --rsp_outstanding == 0) rsp_complete_next = true;
      exp_rx[p].pop_front();
      rx_done[p]++;
    }
    TB_CHECK((bool)dut->ev_err_o == want_err, "t=%llu error event %d, expected %d", (unsigned long long)t,
             dut->ev_err_o, want_err);
    TB_CHECK((bool)dut->doorbell_o == want_bell, "t=%llu doorbell %d, expected %d", (unsigned long long)t,
             dut->doorbell_o, want_bell);
    bool rd_valid = dut->reg_rvalid_o;
    uint32_t rd_data = dut->reg_rdata_o;
    for (int p = 0; p < 2; p++) {
      if (!(dut->inj_valid_o >> p & 1)) continue;
      Flit f = get_flit(dut->inj_flit_o, p);
      int la = (dut->inj_la_o >> (2 * p)) & 3;
      TB_CHECK(la == (int)route(kMeX, kMeY, f.dst_x, f.dst_y), "t=%llu plane %d lookahead port %d",
               (unsigned long long)t, p, la);
      if (f.op == Op::ReadRsp) {
        TB_CHECK(!exp_rep.empty(), "t=%llu unexpected reply %s", (unsigned long long)t, f.str().c_str());
        auto [e, id] = exp_rep.front();
        TB_CHECK(f == e.f && p == e.plane, "t=%llu reply %s d=%08x %08x %08x on plane %d, expected %s d=%08x %08x %08x "
                 "on plane %d", (unsigned long long)t, f.str().c_str(), f.data[1], f.data[2], f.data[3], p,
                 e.f.str().c_str(), e.f.data[1], e.f.data[2], e.f.data[3], e.plane);
        exp_rep.pop_front();
        if (--rep_left[id] == 0) {
          rq_open--;
          for (size_t i = 0; i < pending.size(); i++)
            if (pending[i].id == id) { pending.erase(pending.begin() + (long)i); break; }
        }
        rep_flits[p]++;
      } else {
        int ch = f.tag >> 7;
        TB_CHECK(!exp_tx[ch].empty(), "t=%llu unexpected flit on channel %d: %s", (unsigned long long)t, ch,
                 f.str().c_str());
        const TxExp& e = exp_tx[ch].front();
        TB_CHECK(f == e.f && p == e.plane,
                 "t=%llu channel %d flit %s be=%04x d1=%08x on plane %d, expected %s be=%04x d1=%08x on plane %d",
                 (unsigned long long)t, ch, f.str().c_str(), f.be, f.data[1], p, e.f.str().c_str(), e.f.be,
                 e.f.data[1], e.plane);
        exp_tx[ch].pop_front();
        if (f.op == Op::ReadReq) {
          // The network answers: one READ_RSP per 16 bytes, carrying the tag
          for (uint32_t i = 0; i < f.data[1] / 16; i++) {
            Flit r;
            r.src_x = f.dst_x;
            r.src_y = f.dst_y;
            r.dst_x = kMeX;
            r.dst_y = kMeY;
            r.op = Op::ReadRsp;
            r.addr = f.data[0] + 16 * i;
            r.be = 0xffff;
            r.tag = f.tag;
            for (auto& d : r.data) d = (uint32_t)rng();
            rsp_to_send.push_back(r);
          }
        } else if (exp_tx[ch].empty() && busy[ch]) {
          busy[ch] = false;
          done[ch]++;
        }
        tx_flits[p]++;
      }
      inj_credit_delay[p].push_back(inj_credit_lat + (int)(rng() % (inj_credit_jitter + 1)));
    }
    for (int p = 0; p < 2; p++)
      if (dut->ej_credit_o >> p & 1) ej_credits[p]++;
    if (reg_read)
      for (int c = 0; c < 2; c++) {
        snap_busy[c] = busy[c];
        snap_err[c] = err[c];
        snap_done[c] = done[c];
      }
    half(1);
    for (auto& cd : inj_credit_delay)
      for (auto& d : cd) d--;
    if (rd_valid) {
      last_read = rd_data;
      read_ready = true;
    }
    t++;
    return ctx->gotError() ? 1 : 0;
  };

  auto run = [&](int n) -> int {
    for (int i = 0; i < n; i++) if (cycle()) return 1;
    return 0;
  };
  auto read_reg = [&](uint32_t addr, uint32_t* v) -> int {
    reg_q.push_back({false, addr, 0});
    read_ready = false;
    for (int i = 0; i < 10 && !read_ready; i++) if (cycle()) return 1;
    *v = last_read;
    return read_ready ? 0 : 1;
  };
  auto issue = [&](int ch, const Cmd& c) {
    uint32_t base = 0x20u * ch;
    reg_q.push_back({true, base + 0x00, c.src});
    reg_q.push_back({true, base + 0x04, c.dst_addr});
    reg_q.push_back({true, base + 0x08, c.xy});
    reg_q.push_back({true, base + 0x0C, c.len});
    reg_q.push_back({true, base + 0x10, c.cmd});
  };
  auto check_status = [&](int ch) -> int {
    uint32_t s = 0;
    if (read_reg(0x20u * ch + 0x14, &s)) return 1;
    TB_CHECK((s & 1) == snap_busy[ch] && ((s >> 1) & 1) == snap_err[ch] && (s >> 16) == snap_done[ch],
             "t=%llu channel %d STATUS %08x, expected busy %d error %d done %u", (unsigned long long)t, ch, s,
             snap_busy[ch], snap_err[ch], snap_done[ch]);
    return 0;
  };
  auto random_xy = [&](bool io_ok) {
    for (;;) {
      uint32_t x = (uint32_t)(rng() % kGX), y = (uint32_t)(rng() % kGY);
      if (y == kGY - 1 && (x != 0 || !io_ok)) continue;
      return x | y << 8;
    }
  };
  auto random_cmd = [&](int ch) {
    Cmd c{};
    int op = (int)(rng() % 10);
    if (op < 6 || ch == 1) {  // WRITE (channel 1 also gets ATOMIC_INC)
      c.len = 16 * (1 + (uint32_t)(rng() % 64));
      if (rng() % 32 == 0) c.len = 0;
      c.src = 16 * (uint32_t)(rng() % ((kRegionB - c.len) / 16));
      c.dst_addr = (uint32_t)rng() & ~15u;
      c.xy = random_xy(true);
      c.cmd = 0;
      if (ch == 1 && op >= 6) op = 9;
    } else if (op < 8) {  // READ_REQ, landing in the B2 half of the plane its replies come on
      c.len = 16 * (1 + (uint32_t)(rng() % 32));
      c.dst_addr = (uint32_t)rng() & ~15u;
      c.xy = random_xy(false);
      int rp = plane((int)(c.xy & 0xff), (int)(c.xy >> 8), kMeX, kMeY);
      c.src = b2(rp) + 16 * (uint32_t)(rng() % ((kHalf - c.len) / 16 + 1));
      c.cmd = 1;
    }
    if (op >= 8) {  // ATOMIC_INC with an increment, sometimes the doorbell
      c.dst_addr = (uint32_t)rng() & ~3u;
      c.xy = random_xy(true);
      c.len = (uint32_t)rng();  // ignored
      c.cmd = 2 | (rng() % 4 == 0 ? 1u << 5 : 0) | (uint32_t)(rng() & 0xffff) << 16;
    }
    if (rng() % 6 == 0) {  // force a plane (tests only)
      c.cmd |= (1u + (uint32_t)(rng() % 2)) << 3;
      n_forced++;
    }
    bool bad = rng() % 8 == 0;
    if (bad) {
      switch (rng() % 10) {
        case 0: c = {16, 0, 0, 16, 0}; c.src += 4; break;
        case 1: c = {16, 8, 0, 16, 0}; break;
        case 2: c = {16, 0, 0, 20, 0}; break;
        case 3: c = {16, 0, kGX, 16, 0}; break;                     // outside the grid
        case 4: c = {16, 0, 1u | (kGY - 1u) << 8, 16, 0}; break;     // router-only stop
        case 5: c = {16, 0, 0, 16, 3}; break;                       // reserved op
        case 6: c = {16, 0, 0, 16, 3u << 3}; break;                 // reserved plane
        case 7:  // READ_REQ on channel 1, or (channel 0) to the I/O tile
          c = {kRegionB2, 0, ch == 1 ? 0x0101u : (kGY - 1u) << 8, 16, 1};
          break;
        case 8: c = {0, 6, 0, 0, 2}; break;                         // atomic not word-aligned
        default: c = {kSpm - 16, 0, 0, 32, 1}; break;               // read landing past the end (or write source)
      }
    }
    return std::make_pair(c, bad);
  };

  // ------------------------------------------------- 1. random everything
  for (uint64_t round = 0; round < args.cycles / 200; round++) {
    for (int ch = 0; ch < 2; ch++) {
      if (busy[ch] || rng() % 3) continue;
      auto [c, bad] = random_cmd(ch);
      issue(ch, c);
      cmds++;
      if (bad) {
        err[ch] = true;
        done[ch]++;
        bad_cmds++;
      } else if ((c.cmd & 7) != 2 && c.len == 0) {
        err[ch] = false;
        done[ch]++;
      } else {
        err[ch] = false;
        busy[ch] = true;
        expect_cmd(ch, c);
      }
    }
    if (run(200)) return 1;
    for (int ch = 0; ch < 2; ch++) if (check_status(ch)) return 1;
    // A command while busy is rejected and leaves the running one alone (only
    // while enough work is left that the channel cannot finish first)
    if (busy[0] && (exp_tx[0].size() > 3 || rsp_outstanding > 3) && rng() % 4 == 0) {
      reg_q.push_back({true, 0x10, 0});
      err[0] = true;
      if (run(2)) return 1;
      if (check_status(0)) return 1;
    }
  }
  // Drain
  auto drain = [&]() -> int {
    rx_random = false;
    auto active = [&]() {
      return busy[0] || busy[1] || rsp_complete_next || !exp_rx[0].empty() || !exp_rx[1].empty() ||
             !exp_rep.empty() || !directed[0].empty() || !directed[1].empty();
    };
    for (int i = 0; i < 20000 && active(); i++)
      if (cycle()) return 1;
    TB_CHECK(!active() && exp_tx[0].empty() && exp_tx[1].empty(),
             "did not drain: busy %d %d, %zu+%zu RX records, %zu/%zu flits, %zu replies", busy[0], busy[1],
             exp_rx[0].size(), exp_rx[1].size(), exp_tx[0].size(), exp_tx[1].size(), exp_rep.size());
    return 0;
  };
  if (drain()) return 1;
  for (int ch = 0; ch < 2; ch++) if (check_status(ch)) return 1;

  // ------------------------------------- 2. READ_REQ storm, injection blocked
  // A full queue of READ_REQs, then writes, on both planes: RX must retire
  // every one of them at once although no reply can leave (SPEC.md: ejection
  // never waits on injection).
  contention_pct = 0;
  inj_block = true;
  auto mkflit = [&](Op op, uint32_t addr) {
    Flit f;
    f.src_x = (uint8_t)(rng() % kGX);
    f.src_y = (uint8_t)(rng() % (kGY - 1));
    f.dst_x = kMeX;
    f.dst_y = kMeY;
    f.op = op;
    f.addr = addr;
    f.be = 0xffff;
    f.tag = (uint8_t)rng();
    for (auto& d : f.data) d = (uint32_t)rng();
    return f;
  };
  {
    // First a READ_REQ whose replies use up every injection credit of their
    // plane and fill the reply staging buffer; from then on nothing can leave
    Flit first = mkflit(Op::ReadReq, 0);
    first.data[0] = 0;
    first.data[1] = 1024;
    directed[0].push_back(first);
    if (run(40)) return 1;
    for (int i = 0; i < kRQ; i++) {
      Flit f = mkflit(Op::ReadReq, 0x1000u * (uint32_t)i);
      f.data[0] = 0x100u * (uint32_t)i;
      f.data[1] = 256;
      directed[i % 2].push_back(f);
    }
    for (int i = 0; i < 40; i++) {
      int p = i % 2;
      directed[p].push_back(mkflit(Op::Write, b2(p) + 16u * (uint32_t)i));
    }
    uint64_t rx0 = rx_done[0] + rx_done[1], t0 = t;
    int n = kRQ + 40;
    for (int i = 0; i < 400 && rx_done[0] + rx_done[1] - rx0 < (uint64_t)n; i++) if (cycle()) return 1;
    uint64_t got = rx_done[0] + rx_done[1] - rx0;
    TB_CHECK(got == (uint64_t)n, "storm: retired %llu of %d flits with injection blocked", (unsigned long long)got, n);
    // Retiring at the ejection rate: one flit per cycle per plane once the
    // streams run, the READ_REQs taking turns at the queue
    TB_CHECK(t - t0 <= (uint64_t)(n / 2 + kRQ + 2 * kDepth + 4), "storm: %d flits took %llu cycles", n,
             (unsigned long long)(t - t0));
    printf("storm: %d READ_REQs (queue full) and 40 writes on two planes retired in %llu cycles, injection "
           "blocked\n", kRQ, (unsigned long long)(t - t0));
    inj_block = false;
    if (drain()) return 1;
  }

  // ----------------------------------------------- 3. full rate, no contention
  // tx / rep: the plane the DMA stream (forced, to itself) and the reply stream
  // (to a requester in that direction) inject on, -1 for none; rx: bit mask of
  // the planes receiving a write stream.
  inj_credit_lat = 2;
  inj_credit_jitter = 0;
  auto measure = [&](int tx, int rx, int rep, const char* what) -> int {
    if (tx >= 0) {
      Cmd c{0, 0, (uint32_t)(kMeX | kMeY << 8), 16 * 1000, (uint32_t)(tx + 1) << 3};
      err[0] = false;
      busy[0] = true;
      issue(0, c);
      expect_cmd(0, c);
    }
    if (rep >= 0) {
      Flit f = mkflit(Op::ReadReq, 0);
      f.src_x = (uint8_t)(rep == 0 ? kMeX + 1 : kMeX - 1);   // east: NoC0, west: NoC1
      f.src_y = kMeY;
      f.data[0] = 0;
      f.data[1] = 16384;  // longer than the measurement
      directed[0].push_back(f);
    }
    auto feed = [&](int i) {
      for (int p = 0; p < 2; p++)
        if (rx >> p & 1) directed[p].push_back(mkflit(Op::Write, b1(p) + 16u * (uint32_t)(i % 512)));
    };
    for (int i = 0; i < 100; i++) {
      feed(i);
      if (cycle()) return 1;
    }
    uint64_t tx0 = tx_flits[0] + tx_flits[1], rp0 = rep_flits[0] + rep_flits[1], r0[2] = {rx_done[0], rx_done[1]};
    for (int i = 0; i < 200; i++) {
      feed(i);
      if (cycle()) return 1;
    }
    double rt = (tx_flits[0] + tx_flits[1] - tx0) / 200.0, rp = (rep_flits[0] + rep_flits[1] - rp0) / 200.0;
    double rr[2] = {(rx_done[0] - r0[0]) / 200.0, (rx_done[1] - r0[1]) / 200.0};
    if (drain()) return 1;
    bool ok = true;
    for (int p = 0; p < 2; p++) ok &= !(rx >> p & 1) || rr[p] > 0.99;
    if (tx >= 0 && rep >= 0 && tx == rep) ok &= rt + rp > 0.99 && rt > 0.45 && rp > 0.45;
    else ok &= (tx < 0 || rt > 0.99) && (rep < 0 || rp > 0.99);
    TB_CHECK(ok, "%s: injected %.3f (DMA) + %.3f (replies), retired %.3f + %.3f flits/cycle", what, rt, rp, rr[0],
             rr[1]);
    printf("full rate %-22s injected %.3f (DMA) + %.3f (replies), retired %.3f (RX0) + %.3f (RX1) flits/cycle\n",
           what, rt, rp, rr[0], rr[1]);
    return 0;
  };
  if (measure(0, 0, -1, "TX NoC0")) return 1;
  if (measure(1, 0, -1, "TX NoC1")) return 1;
  if (measure(-1, 1, -1, "RX0")) return 1;
  if (measure(-1, 2, -1, "RX1")) return 1;
  if (measure(-1, 3, -1, "RX0+RX1")) return 1;
  if (measure(-1, 0, 0, "reply NoC0")) return 1;
  if (measure(-1, 0, 1, "reply NoC1")) return 1;
  if (measure(0, 0, 0, "TX+reply NoC0")) return 1;
  if (measure(0, 0, 1, "TX NoC0, reply NoC1")) return 1;
  if (measure(1, 0, 0, "TX NoC1, reply NoC0")) return 1;
  if (measure(0, 1, -1, "TX+RX0")) return 1;
  if (measure(-1, 2, 0, "RX1+reply")) return 1;
  if (measure(0, 3, 1, "TX, reply, RX0, RX1")) return 1;

  // ------------------------------------------------------- final memory
  for (uint32_t a = 0; a < kSpm; a += 4) {
    uint32_t w = bd_read32(a), m;
    memcpy(&m, &mem[a], 4);
    TB_CHECK(w == m, "memory at %04x: rtl %08x model %08x", a, w, m);
  }
  if (tfp) tfp->close();
  printf("tb_niu: PASS commands=%llu (%llu bad, %llu forced plane) tx_flits=%llu+%llu reply_flits=%llu+%llu "
         "rx_flits=%llu+%llu (read_reqs=%llu atomics=%llu reg_writes=%llu dropped=%llu)\n",
         (unsigned long long)cmds, (unsigned long long)bad_cmds, (unsigned long long)n_forced,
         (unsigned long long)tx_flits[0], (unsigned long long)tx_flits[1], (unsigned long long)rep_flits[0],
         (unsigned long long)rep_flits[1], (unsigned long long)rx_done[0], (unsigned long long)rx_done[1],
         (unsigned long long)n_readreq, (unsigned long long)n_atomic, (unsigned long long)n_regwr,
         (unsigned long long)n_drop);
  return 0;
}
