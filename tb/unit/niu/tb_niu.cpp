// NIU unit test (SPEC.md, Verification strategy: NIU and DMA).
//
// The NIU sits on its real scratchpad. The testbench drives the registers, plays
// the network on both sides and adds random core reads for bank conflicts. A
// DMA model predicts every injected flit from a memory model; RX flits are
// predicted as an ordered list of actions (scratchpad write, register write,
// dropped error) and the NIU must perform them in exactly that order, whatever
// the bank contention. Checks:
//   - TX: per channel, flits in address order with the right header, data,
//     tag and lookahead port; STATUS busy/done/error; every command error;
//   - RX: byte enables, in-order retire across targets, errors dropped;
//   - memory at the end equals the model;
//   - full rate: one flit per cycle injected, ejected, and both at once.
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

constexpr int kGX = 4, kGY = 4, kDepth = 5, kX = 1, kY = 2;
constexpr uint32_t kSpm = 64 * 1024, kRegionB = 0x8000;  // DMA sources below, RX writes above
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

struct Cmd {
  uint32_t src, dst_addr, xy, len, cmd;
};

enum class Act { SpmWrite, RegWrite, Drop };
struct RxAction {
  Act kind;
  uint32_t addr, wdata;
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
  const int me_x = 1, me_y = 2;
  dut->x_i = me_x;
  dut->y_i = me_y;
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
  std::deque<Flit> exp_tx[2];       // expected flits per channel
  uint8_t seq[2] = {0, 0};
  bool busy[2] = {false, false};
  uint16_t done[2] = {0, 0};
  bool err[2] = {false, false};
  std::deque<RxAction> exp_rx;
  std::deque<std::pair<int, uint32_t>> credit_return;  // (cycles left, unused)
  std::deque<int> inj_credit_delay;
  int ej_credits = kDepth;
  uint64_t t = 0, tx_flits = 0, rx_done = 0, cmds = 0, bad_cmds = 0;

  // Register traffic: one op per cycle; reads report through a callback
  struct RegOp {
    bool we;
    uint32_t addr, wdata;
    int tag;
  };
  std::deque<RegOp> reg_q;
  int read_tag = -1;
  uint32_t last_read = 0;
  bool read_ready = false;

  // Network behaviour knobs per phase
  int inj_credit_lat = 2, ej_send_pct = 50, contention_pct = 30, inj_credit_jitter = 4;
  bool rx_stream = false;          // full-rate phases: RX writes consecutive words, like a remote DMA
  uint32_t rx_stream_addr = kRegionB;

  // Expected flits of a WRITE command, from the model memory
  auto expect_cmd = [&](int ch, const Cmd& c) {
    for (uint32_t i = 0; i < c.len / 16; i++) {
      Flit f;
      f.dst_x = (uint8_t)(c.xy & 0xff);
      f.dst_y = (uint8_t)((c.xy >> 8) & 0xff);
      f.src_x = me_x;
      f.src_y = me_y;
      f.op = Op::Write;
      f.addr = c.dst_addr + 16 * i;
      f.be = 0xffff;
      f.tag = (uint8_t)(ch << 7 | seq[ch]);
      seq[ch] = (seq[ch] + 1) & 0x7f;
      memcpy(f.data, &mem[c.src + 16 * i], 16);
      exp_tx[ch].push_back(f);
    }
  };

  auto cycle = [&]() -> int {
    // ---- drive inputs
    // Register port
    dut->reg_req_i = 0;
    if (!reg_q.empty()) {
      RegOp op = reg_q.front();
      reg_q.pop_front();
      dut->reg_req_i = 1;
      dut->reg_we_i = op.we;
      dut->reg_addr_i = op.addr;
      dut->reg_wdata_i = op.wdata;
      read_tag = op.we ? -1 : op.tag;
    } else {
      read_tag = -1;
    }
    // Injection credits come back after a delay
    bool cr = false;
    if (!inj_credit_delay.empty() && inj_credit_delay.front() <= 0) {
      cr = true;
      inj_credit_delay.pop_front();
    }
    dut->inj_credit_i = cr;
    // Ejection: send a random flit within the NIU's credits
    dut->ej_valid_i = 0;
    Flit ef;
    bool sending = ej_credits > 0 && (int)(rng() % 100) < ej_send_pct;
    if (sending) {
      int kind = (int)(rng() % 100);
      ef.op = Op::Write;
      ef.src_x = (uint8_t)(rng() % kGX);
      ef.src_y = (uint8_t)(rng() % kGY);
      ef.dst_x = me_x;
      ef.dst_y = me_y;
      ef.tag = (uint8_t)rng();
      for (auto& d : ef.data) d = (uint32_t)rng();
      ef.be = (uint16_t)rng();
      if (rx_stream) {
        kind = 0;
        rx_stream_addr = rx_stream_addr + 16 >= kSpm ? kRegionB : rx_stream_addr + 16;
      }
      if (kind < 85) {  // scratchpad write anywhere in region B
        ef.addr = rx_stream ? rx_stream_addr
                            : kRegionB + ((uint32_t)rng() % (kSpm - kRegionB) & ~15u) + (uint32_t)(rng() & 15);
        exp_rx.push_back({Act::SpmWrite, ef.addr & 0xfff0, 0});
        // model: apply now (the NIU applies in order; the final check compares)
        for (int b = 0; b < 16; b++)
          if (ef.be >> b & 1) mem[(ef.addr & 0xfff0) + b] = ((uint8_t*)ef.data)[b];
      } else if (kind < 93) {  // register write (tile control / counter bank)
        ef.addr = (rng() & 1 ? kTctrl : kPerf) + ((uint32_t)rng() & 0xffc);
        uint32_t lane = (ef.addr >> 2) & 3;
        exp_rx.push_back({Act::RegWrite, ef.addr, ef.data[lane]});
      } else if (kind < 97) {  // unsupported op: dropped
        ef.op = Op::ReadReq;
        ef.addr = kRegionB;
        exp_rx.push_back({Act::Drop, 0, 0});
      } else {  // unmapped address: dropped
        ef.addr = 0x20000000u | ((uint32_t)rng() & 0xfff0);
        exp_rx.push_back({Act::Drop, 0, 0});
      }
      uint32_t w[Flit::kWords];
      ef.pack(w);
      for (int k = 0; k < Flit::kWords; k++) dut->ej_flit_i[k] = w[k];
      dut->ej_valid_i = 1;
      ej_credits--;
    }
    // Core contention
    dut->cd_req_i = (int)(rng() % 100) < contention_pct;
    dut->cd_addr_i = (uint16_t)rng();
    dut->ci_req_i = (int)(rng() % 100) < contention_pct;
    dut->ci_addr_i = (uint16_t)rng();
    half(0);

    // ---- observe this cycle (combinational outputs)
    if (dut->rx_wr_o) {
      TB_CHECK(!exp_rx.empty(), "t=%llu unexpected RX scratchpad write", (unsigned long long)t);
      RxAction a = exp_rx.front();
      TB_CHECK(a.kind == Act::SpmWrite && a.addr == (dut->rx_wr_addr_o & 0xfff0u),
               "t=%llu RX out of order: scratchpad write at %04x, expected kind %d addr %08x",
               (unsigned long long)t, dut->rx_wr_addr_o, (int)a.kind, a.addr);
    }
    if (dut->rmt_req_o) {
      TB_CHECK(!exp_rx.empty(), "t=%llu unexpected register write", (unsigned long long)t);
      RxAction a = exp_rx.front();
      TB_CHECK(a.kind == Act::RegWrite && a.addr == dut->rmt_addr_o && a.wdata == dut->rmt_wdata_o,
               "t=%llu RX out of order: register write %08x=%08x, expected kind %d %08x=%08x",
               (unsigned long long)t, dut->rmt_addr_o, dut->rmt_wdata_o, (int)a.kind, a.addr, a.wdata);
    }
    if (dut->ev_err_o) {
      TB_CHECK(!exp_rx.empty() && exp_rx.front().kind == Act::Drop, "t=%llu unexpected drop",
               (unsigned long long)t);
    }
    if (dut->ev_rx_o) {
      TB_CHECK(!exp_rx.empty(), "t=%llu RX retired with nothing expected", (unsigned long long)t);
      exp_rx.pop_front();
      rx_done++;
    }
    bool rd_valid = dut->reg_rvalid_o;
    uint32_t rd_data = dut->reg_rdata_o;
    // Injected flit (registered output)
    if (dut->inj_valid_o) {
      uint32_t w[Flit::kWords];
      for (int k = 0; k < Flit::kWords; k++) w[k] = dut->inj_flit_o[k];
      Flit f = Flit::unpack(w);
      int ch = f.tag >> 7;
      TB_CHECK(!exp_tx[ch].empty(), "t=%llu unexpected flit on channel %d: %s", (unsigned long long)t, ch,
               f.str().c_str());
      TB_CHECK(f == exp_tx[ch].front(), "t=%llu channel %d flit %s, expected %s", (unsigned long long)t, ch,
               f.str().c_str(), exp_tx[ch].front().str().c_str());
      TB_CHECK(dut->inj_la_o == (int)route(me_x, me_y, f.dst_x, f.dst_y), "t=%llu lookahead port %d",
               (unsigned long long)t, dut->inj_la_o);
      exp_tx[ch].pop_front();
      if (exp_tx[ch].empty() && busy[ch]) {
        busy[ch] = false;
        done[ch]++;
      }
      tx_flits++;
      // The network frees the slot after a delay
      inj_credit_delay.push_back(inj_credit_lat + (int)(rng() % (inj_credit_jitter + 1)));
    }
    if (dut->ej_credit_o) ej_credits++;
    half(1);
    for (auto& d : inj_credit_delay) d--;
    // Reads complete one cycle after their request
    if (rd_valid) {
      last_read = rd_data;
      read_ready = true;
    }
    (void)read_tag;
    t++;
    return ctx->gotError() ? 1 : 0;
  };

  auto run = [&](int n) -> int {
    for (int i = 0; i < n; i++) if (cycle()) return 1;
    return 0;
  };
  auto read_reg = [&](uint32_t addr, uint32_t* v) -> int {
    reg_q.push_back({false, addr, 0, 1});
    read_ready = false;
    for (int i = 0; i < 10 && !read_ready; i++) if (cycle()) return 1;
    *v = last_read;
    return read_ready ? 0 : 1;
  };
  auto issue = [&](int ch, const Cmd& c) {
    uint32_t base = 0x20u * ch;
    reg_q.push_back({true, base + 0x00, c.src, 0});
    reg_q.push_back({true, base + 0x04, c.dst_addr, 0});
    reg_q.push_back({true, base + 0x08, c.xy, 0});
    reg_q.push_back({true, base + 0x0C, c.len, 0});
    reg_q.push_back({true, base + 0x10, c.cmd, 0});
  };
  auto check_status = [&](int ch) -> int {
    uint32_t s = 0;
    if (read_reg(0x20u * ch + 0x14, &s)) return 1;
    TB_CHECK((s & 1) == busy[ch] && ((s >> 1) & 1) == err[ch] && (s >> 16) == done[ch],
             "t=%llu channel %d STATUS %08x, expected busy %d error %d done %u", (unsigned long long)t, ch, s,
             busy[ch], err[ch], done[ch]);
    return 0;
  };
  auto random_cmd = [&](int ch) {
    Cmd c;
    c.len = 16 * (1 + (uint32_t)(rng() % 64));
    c.src = 16 * (uint32_t)(rng() % ((kRegionB - c.len) / 16));
    c.dst_addr = (uint32_t)rng() & ~15u;
    c.xy = (uint32_t)(rng() % kGX) | (uint32_t)(rng() % kGY) << 8;
    c.cmd = 0;
    bool bad = rng() % 8 == 0;
    if (bad) {
      switch (rng() % 6) {
        case 0: c.src += 4; break;
        case 1: c.dst_addr += 8; break;
        case 2: c.len += 4; break;
        case 3: c.xy = kGX; break;
        case 4: c.cmd = 1; break;               // READ_REQ: M2
        default: c.cmd = 2u << 3; break;        // NoC1 forced: M3
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
      } else {
        err[ch] = false;
        busy[ch] = true;
        expect_cmd(ch, c);
      }
    }
    if (run(200)) return 1;
    for (int ch = 0; ch < 2; ch++) if (check_status(ch)) return 1;
    // A command while busy is rejected and leaves the running one alone
    if (busy[0] && rng() % 4 == 0) {
      reg_q.push_back({true, 0x10, 0, 0});
      err[0] = true;
      if (run(2)) return 1;
      if (check_status(0)) return 1;
    }
  }
  // Drain
  ej_send_pct = 0;
  for (int i = 0; i < 5000 && (busy[0] || busy[1] || !exp_rx.empty()); i++) if (cycle()) return 1;
  TB_CHECK(!busy[0] && !busy[1] && exp_rx.empty() && exp_tx[0].empty() && exp_tx[1].empty(),
           "did not drain: busy %d %d, %zu RX actions, %zu/%zu flits", busy[0], busy[1], exp_rx.size(),
           exp_tx[0].size(), exp_tx[1].size());
  for (int ch = 0; ch < 2; ch++) if (check_status(ch)) return 1;

  // ----------------------------------------------- 2. full rate, no contention
  contention_pct = 0;
  rx_stream = true;
  inj_credit_lat = 2;
  inj_credit_jitter = 0;
  auto measure = [&](bool tx, bool rx, const char* what) -> int {
    if (tx) {
      Cmd c{0, 0, (uint32_t)(me_x | me_y << 8), 0x8000 - 0x10, 0};
      c.len = 16 * 1000;
      err[0] = false;
      busy[0] = true;
      issue(0, c);
      expect_cmd(0, c);
    }
    ej_send_pct = rx ? 100 : 0;
    if (run(100)) return 1;
    uint64_t tx0 = tx_flits, rx0 = rx_done;
    if (run(500)) return 1;
    double rt = (tx_flits - tx0) / 500.0, rr = (rx_done - rx0) / 500.0;
    ej_send_pct = 0;
    for (int i = 0; i < 3000 && (busy[0] || !exp_rx.empty()); i++) if (cycle()) return 1;
    TB_CHECK((!tx || rt > 0.99) && (!rx || rr > 0.99), "%s: injected %.3f, retired %.3f flits/cycle", what,
             rt, rr);
    printf("full rate %s: injected %.3f, retired %.3f flits/cycle\n", what, rt, rr);
    return 0;
  };
  if (measure(true, false, "TX")) return 1;
  if (measure(false, true, "RX")) return 1;
  if (measure(true, true, "TX+RX")) return 1;

  // ------------------------------------------------------- final memory
  for (uint32_t a = kRegionB; a < kSpm; a += 4) {
    uint32_t w = bd_read32(a), m;
    memcpy(&m, &mem[a], 4);
    TB_CHECK(w == m, "memory at %04x: rtl %08x model %08x", a, w, m);
  }
  if (tfp) tfp->close();
  printf("tb_niu: PASS commands=%llu (%llu bad) tx_flits=%llu rx_flits=%llu\n", (unsigned long long)cmds,
         (unsigned long long)bad_cmds, (unsigned long long)tx_flits, (unsigned long long)rx_done);
  return 0;
}
