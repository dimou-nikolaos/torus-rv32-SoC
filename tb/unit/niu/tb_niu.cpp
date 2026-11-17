// NIU unit test (SPEC.md, Verification strategy: NIU and DMA).
//
// The NIU sits on its real scratchpad. The testbench drives the registers, plays
// the network on both sides, models tile control and the counter bank on the
// remote register port, and adds random core reads for bank conflicts. A DMA
// model predicts every injected flit from a memory model; RX flits are predicted
// as an ordered list of records (scratchpad write, register writes, READ_REQ
// into the queue, atomic, dropped error) that the NIU must perform in exactly
// that order, whatever the bank contention; every accepted READ_REQ predicts
// its reply flits. Checks:
//   - TX: per channel, flits in address order with the right header, data,
//     tag and lookahead port, for WRITE, READ_REQ and ATOMIC_INC; STATUS
//     busy/done/error, a read completing only when its last reply landed; every
//     command error;
//   - RX: byte enables, register writes per lane, in-order retire across
//     targets, atomics as locked adds, the doorbell, errors dropped;
//   - TX-reply: replies in order with the data the source held when the
//     READ_REQ retired (rule 2), from the scratchpad, registers or zeros;
//   - READ_REQ storm: with injection blocked, a full queue of READ_REQs and
//     more writes retire without RX ever stalling;
//   - memory at the end equals the model;
//   - full rate: one flit per cycle injected, ejected, replied, and shared by
//     DMA and replies.
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
#include "router_model.h"
#include "tb_args.h"

using namespace torus;

namespace {

constexpr int kGX = 4, kGY = 4, kDepth = 5, kRQ = 6;  // row kGY-1 is the I/O row
constexpr int kMeX = 1, kMeY = 2;
// Region A: DMA and reply sources, never written. B1: RX writes and reply
// sources. B2: RX writes and the landing area of our own reads.
constexpr uint32_t kSpm = 64 * 1024, kRegionB = 0x8000, kRegionB2 = 0xC000;
constexpr uint32_t kTctrl = 0x10001000, kPerf = 0x10002000;

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

struct Cmd {
  uint32_t src, dst_addr, xy, len, cmd;
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
  std::deque<Flit> exp_tx[2];       // expected DMA flits per channel
  std::deque<std::pair<Flit, int>> exp_rep;  // expected reply flits, with their request id
  std::vector<Range> pending;       // reply sources not fully sent yet: RX writes avoid them
  std::vector<int> rep_left;        // per request id: reply flits not yet seen
  uint8_t seq[2] = {0, 0};
  bool busy[2] = {false, false};
  uint16_t done[2] = {0, 0};
  bool err[2] = {false, false};
  int rsp_outstanding = 0;          // our READ_REQ: replies not yet retired
  uint8_t rsp_tag = 0;
  std::deque<Flit> rsp_to_send;     // the network's answer to our READ_REQ
  std::deque<RxRec> exp_rx;
  std::deque<Flit> directed;        // RX flits a phase wants sent, before random ones
  std::deque<int> inj_credit_delay;
  int ej_credits = kDepth;
  bool inj_block = false;
  int rq_open = 0;                  // READ_REQs sent whose replies are not all out
  uint64_t t = 0, tx_flits = 0, rep_flits = 0, rx_done = 0, cmds = 0, bad_cmds = 0, n_atomic = 0,
           n_readreq = 0, n_regwr = 0, n_drop = 0;

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
    uint32_t op = c.cmd & 7;
    uint32_t n = op == 0 ? c.len / 16 : 1;
    for (uint32_t i = 0; i < n; i++) {
      Flit f;
      f.dst_x = (uint8_t)(c.xy & 0xff);
      f.dst_y = (uint8_t)((c.xy >> 8) & 0xff);
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
      } else {
        f.be = (uint16_t)(0xF << (4 * ((c.dst_addr >> 2) & 3)));
        f.data[0] = c.cmd >> 16;
        f.data[1] = (c.cmd >> 5) & 1;
      }
      exp_tx[ch].push_back(f);
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
          exp_rep.push_back({rf, id});
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

  // A random RX flit (the model is applied when it is sent)
  auto random_rx = [&]() {
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
        uint32_t a = kRegionB + ((uint32_t)rng() % (kSpm - kRegionB - len + 1) & ~15u);
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
      else if (src < 70) f.addr = kRegionB + ((uint32_t)rng() % (kRegionB2 - kRegionB - len + 1) & ~15u);  // B1
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
      case 0: f.op = Op::Multicast; f.addr = kRegionB; break;
      case 1: f.op = Op::Write; f.addr = 0x20000000u | ((uint32_t)rng() & 0xfff0); break;
      case 2: f.op = Op::AtomicInc; f.addr = kRegionB + 2; break;
      default: f.op = Op::ReadRsp; f.addr = kRegionB; f.tag |= 0x80; break;  // channel 0 tags have bit 7 clear
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
    bool cr = false;
    if (!inj_block && !inj_credit_delay.empty() && inj_credit_delay.front() <= 0) {
      cr = true;
      inj_credit_delay.pop_front();
    }
    dut->inj_credit_i = cr;
    // Remote register answer to last cycle's read
    dut->rmt_rvalid_i = rmt_rd_pending;
    dut->rmt_rdata_i = rmt_rd_pending && !reg_err(rmt_rd_addr) ? reg_val(rmt_rd_addr) : 0;
    dut->rmt_err_i = rmt_rd_pending && reg_err(rmt_rd_addr);
    // Ejection: our replies, directed flits, then random traffic, within the NIU's credits
    dut->ej_valid_i = 0;
    if (ej_credits > 0) {
      Flit f;
      bool send = false;
      if (!rsp_to_send.empty() && rng() % 2 == 0) {
        f = rsp_to_send.front();
        rsp_to_send.pop_front();
        send = true;
      } else if (!directed.empty()) {
        f = directed.front();
        directed.pop_front();
        send = true;
      } else if (rx_random && (int)(rng() % 100) < ej_send_pct) {
        f = random_rx();
        send = true;
      }
      if (send) {
        exp_rx.push_back(classify(f));
        uint32_t w[Flit::kWords];
        f.pack(w);
        for (int k = 0; k < Flit::kWords; k++) dut->ej_flit_i[k] = w[k];
        dut->ej_valid_i = 1;
        ej_credits--;
      }
    }
    dut->cd_req_i = (int)(rng() % 100) < contention_pct;
    dut->cd_addr_i = (uint16_t)rng();
    dut->ci_req_i = (int)(rng() % 100) < contention_pct;
    dut->ci_addr_i = (uint16_t)rng();
    half(0);

    // ---- observe this cycle (combinational outputs)
    if (dut->rx_wr_o) {
      TB_CHECK(!exp_rx.empty(), "t=%llu unexpected RX scratchpad write", (unsigned long long)t);
      const RxRec& a = exp_rx.front();
      bool is_atomic = a.kind == Act::Atomic;
      TB_CHECK((a.kind == Act::SpmWrite || is_atomic) && a.addr == (dut->rx_wr_addr_o & 0xfff0u) &&
                   (bool)dut->rx_wr_add_o == is_atomic,
               "t=%llu RX out of order: scratchpad write at %04x (add %d), expected kind %d addr %08x",
               (unsigned long long)t, dut->rx_wr_addr_o, dut->rx_wr_add_o, (int)a.kind, a.addr);
    }
    rmt_rd_pending = false;
    if (dut->rmt_req_o && dut->rmt_we_o) {
      TB_CHECK(!exp_rx.empty() && exp_rx.front().kind == Act::RegWrite && !exp_rx.front().regs.empty(),
               "t=%llu unexpected register write %08x", (unsigned long long)t, dut->rmt_addr_o);
      RegWr e = exp_rx.front().regs.front();
      exp_rx.front().regs.pop_front();
      TB_CHECK(e.addr == dut->rmt_addr_o && e.wdata == dut->rmt_wdata_o && e.be == dut->rmt_be_o,
               "t=%llu register write %08x=%08x be %x, expected %08x=%08x be %x", (unsigned long long)t,
               dut->rmt_addr_o, dut->rmt_wdata_o, dut->rmt_be_o, e.addr, e.wdata, e.be);
    } else if (dut->rmt_req_o) {
      TB_CHECK(dut->rmt_be_o == 0xF && (dut->rmt_addr_o & 3) == 0, "t=%llu bad register read", (unsigned long long)t);
      rmt_rd_pending = true;
      rmt_rd_addr = dut->rmt_addr_o;
    }
    bool retire = dut->ev_rx_o;
    if (dut->ev_err_o)
      TB_CHECK(retire && !exp_rx.empty() && exp_rx.front().err, "t=%llu unexpected error event", (unsigned long long)t);
    if (retire) {
      TB_CHECK(!exp_rx.empty(), "t=%llu RX retired with nothing expected", (unsigned long long)t);
      const RxRec& a = exp_rx.front();
      TB_CHECK(a.regs.empty(), "t=%llu retired with %zu register writes missing", (unsigned long long)t, a.regs.size());
      TB_CHECK((bool)dut->ev_err_o == a.err, "t=%llu retire of kind %d: error event %d", (unsigned long long)t,
               (int)a.kind, dut->ev_err_o);
      TB_CHECK((bool)dut->doorbell_o == a.doorbell, "t=%llu doorbell %d, expected %d", (unsigned long long)t,
               dut->doorbell_o, a.doorbell);
      if (a.rsp && --rsp_outstanding == 0) rsp_complete_next = true;
      exp_rx.pop_front();
      rx_done++;
    } else {
      TB_CHECK(!dut->doorbell_o, "t=%llu doorbell without a retire", (unsigned long long)t);
    }
    bool rd_valid = dut->reg_rvalid_o;
    uint32_t rd_data = dut->reg_rdata_o;
    if (dut->inj_valid_o) {
      uint32_t w[Flit::kWords];
      for (int k = 0; k < Flit::kWords; k++) w[k] = dut->inj_flit_o[k];
      Flit f = Flit::unpack(w);
      TB_CHECK(dut->inj_la_o == (int)route(kMeX, kMeY, f.dst_x, f.dst_y), "t=%llu lookahead port %d",
               (unsigned long long)t, dut->inj_la_o);
      if (f.op == Op::ReadRsp) {
        TB_CHECK(!exp_rep.empty(), "t=%llu unexpected reply %s", (unsigned long long)t, f.str().c_str());
        auto [e, id] = exp_rep.front();
        TB_CHECK(f == e, "t=%llu reply %s d=%08x %08x %08x, expected %s d=%08x %08x %08x", (unsigned long long)t,
                 f.str().c_str(), f.data[1], f.data[2], f.data[3], e.str().c_str(), e.data[1], e.data[2], e.data[3]);
        exp_rep.pop_front();
        if (--rep_left[id] == 0) {
          rq_open--;
          for (size_t i = 0; i < pending.size(); i++)
            if (pending[i].id == id) { pending.erase(pending.begin() + (long)i); break; }
        }
        rep_flits++;
      } else {
        int ch = f.tag >> 7;
        TB_CHECK(!exp_tx[ch].empty(), "t=%llu unexpected flit on channel %d: %s", (unsigned long long)t, ch,
                 f.str().c_str());
        TB_CHECK(f == exp_tx[ch].front(), "t=%llu channel %d flit %s be=%04x d1=%08x, expected %s be=%04x d1=%08x",
                 (unsigned long long)t, ch, f.str().c_str(), f.be, f.data[1], exp_tx[ch].front().str().c_str(),
                 exp_tx[ch].front().be, exp_tx[ch].front().data[1]);
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
        tx_flits++;
      }
      inj_credit_delay.push_back(inj_credit_lat + (int)(rng() % (inj_credit_jitter + 1)));
    }
    if (dut->ej_credit_o) ej_credits++;
    if (reg_read)
      for (int c = 0; c < 2; c++) {
        snap_busy[c] = busy[c];
        snap_err[c] = err[c];
        snap_done[c] = done[c];
      }
    half(1);
    for (auto& d : inj_credit_delay) d--;
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
    } else if (op < 8) {  // READ_REQ, landing in B2
      c.len = 16 * (1 + (uint32_t)(rng() % 32));
      c.src = kRegionB2 + 16 * (uint32_t)(rng() % ((kSpm - kRegionB2 - c.len) / 16 + 1));
      c.dst_addr = (uint32_t)rng() & ~15u;
      c.xy = random_xy(false);
      c.cmd = 1;
    }
    if (op >= 8) {  // ATOMIC_INC with an increment, sometimes the doorbell
      c.dst_addr = (uint32_t)rng() & ~3u;
      c.xy = random_xy(true);
      c.len = (uint32_t)rng();  // ignored
      c.cmd = 2 | (rng() % 4 == 0 ? 1u << 5 : 0) | (uint32_t)(rng() & 0xffff) << 16;
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
        case 6: c = {16, 0, 0, 16, 2u << 3}; break;                 // NoC1 forced: M3
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
    for (int i = 0; i < 20000 && (busy[0] || busy[1] || rsp_complete_next || !exp_rx.empty() || !exp_rep.empty() ||
                                  !directed.empty());
         i++)
      if (cycle()) return 1;
    TB_CHECK(!busy[0] && !busy[1] && exp_rx.empty() && exp_tx[0].empty() && exp_tx[1].empty() && exp_rep.empty(),
             "did not drain: busy %d %d, %zu RX records, %zu/%zu flits, %zu replies", busy[0], busy[1],
             exp_rx.size(), exp_tx[0].size(), exp_tx[1].size(), exp_rep.size());
    return 0;
  };
  if (drain()) return 1;
  for (int ch = 0; ch < 2; ch++) if (check_status(ch)) return 1;

  // ------------------------------------- 2. READ_REQ storm, injection blocked
  // A full queue of READ_REQs, then writes: RX must retire every one of them
  // at once although no reply can leave (SPEC.md: ejection never waits on
  // injection).
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
    // First a READ_REQ whose replies use up every injection credit and fill
    // the reply staging buffer; from then on nothing can leave
    Flit first = mkflit(Op::ReadReq, 0);
    first.data[0] = 0;
    first.data[1] = 1024;
    directed.push_back(first);
    if (run(40)) return 1;
    for (int i = 0; i < kRQ; i++) {
      Flit f = mkflit(Op::ReadReq, 0x1000u * (uint32_t)i);
      f.data[0] = 0x100u * (uint32_t)i;
      f.data[1] = 256;
      directed.push_back(f);
    }
    for (int i = 0; i < 40; i++) directed.push_back(mkflit(Op::Write, kRegionB2 + 16u * (uint32_t)i));
    uint64_t rx0 = rx_done, t0 = t;
    int n = kRQ + 40;
    for (int i = 0; i < 400 && rx_done - rx0 < (uint64_t)n; i++) if (cycle()) return 1;
    TB_CHECK(rx_done - rx0 == (uint64_t)n, "storm: retired %llu of %d flits with injection blocked",
             (unsigned long long)(rx_done - rx0), n);
    // Retiring at the ejection rate: one flit per cycle once the stream runs
    TB_CHECK(t - t0 <= (uint64_t)n + 2 * kDepth + 4, "storm: %d flits took %llu cycles", n,
             (unsigned long long)(t - t0));
    printf("storm: %d READ_REQs (queue full) and 40 writes retired in %llu cycles, injection blocked\n", kRQ,
           (unsigned long long)(t - t0));
    inj_block = false;
    if (drain()) return 1;
  }

  // ----------------------------------------------- 3. full rate, no contention
  inj_credit_lat = 2;
  inj_credit_jitter = 0;
  auto measure = [&](bool tx, bool rx, bool rep, const char* what) -> int {
    if (tx) {
      Cmd c{0, 0, (uint32_t)(kMeX | kMeY << 8), 16 * 1000, 0};
      err[0] = false;
      busy[0] = true;
      issue(0, c);
      expect_cmd(0, c);
    }
    if (rep) {
      Flit f = mkflit(Op::ReadReq, 0);
      f.data[0] = 0;
      f.data[1] = 16384;  // longer than the measurement
      directed.push_back(f);
    }
    for (int i = 0; i < 100; i++) {
      if (rx) directed.push_back(mkflit(Op::Write, kRegionB + 16u * (uint32_t)(i % 1000)));
      if (cycle()) return 1;
    }
    uint64_t tx0 = tx_flits, rx0 = rx_done, rp0 = rep_flits;
    for (int i = 0; i < 200; i++) {
      if (rx) directed.push_back(mkflit(Op::Write, kRegionB + 16u * (uint32_t)(i % 1000)));
      if (cycle()) return 1;
    }
    double rt = (tx_flits - tx0) / 200.0, rr = (rx_done - rx0) / 200.0, rp = (rep_flits - rp0) / 200.0;
    if (drain()) return 1;
    bool ok = (!rx || rr > 0.99) && (tx && rep ? rt + rp > 0.99 && rt > 0.45 && rp > 0.45
                                               : (!tx || rt > 0.99) && (!rep || rp > 0.99));
    TB_CHECK(ok, "%s: injected %.3f (DMA) + %.3f (replies), retired %.3f flits/cycle", what, rt, rp, rr);
    printf("full rate %-12s injected %.3f (DMA) + %.3f (replies), retired %.3f flits/cycle\n", what, rt, rp, rr);
    return 0;
  };
  if (measure(true, false, false, "TX")) return 1;
  if (measure(false, true, false, "RX")) return 1;
  if (measure(false, false, true, "reply")) return 1;
  if (measure(true, false, true, "TX+reply")) return 1;
  if (measure(true, true, false, "TX+RX")) return 1;
  if (measure(false, true, true, "RX+reply")) return 1;

  // ------------------------------------------------------- final memory
  for (uint32_t a = 0; a < kSpm; a += 4) {
    uint32_t w = bd_read32(a), m;
    memcpy(&m, &mem[a], 4);
    TB_CHECK(w == m, "memory at %04x: rtl %08x model %08x", a, w, m);
  }
  if (tfp) tfp->close();
  printf("tb_niu: PASS commands=%llu (%llu bad) tx_flits=%llu reply_flits=%llu rx_flits=%llu "
         "(read_reqs=%llu atomics=%llu reg_writes=%llu dropped=%llu)\n",
         (unsigned long long)cmds, (unsigned long long)bad_cmds, (unsigned long long)tx_flits,
         (unsigned long long)rep_flits, (unsigned long long)rx_done, (unsigned long long)n_readreq,
         (unsigned long long)n_atomic, (unsigned long long)n_regwr, (unsigned long long)n_drop);
  return 0;
}
