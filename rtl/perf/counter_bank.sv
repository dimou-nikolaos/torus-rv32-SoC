// Bank of 64-bit event counters, readable from the local core and the remote side.
//
//   0x000 + 8*i   live counter i: lo, then hi (hi latched when lo is read on the
//                 same port, so lo-then-hi is one consistent 64-bit value)
//   0x800 + 8*i   snapshot of counter i: lo, hi (static until the next snapshot)
//   0xFF0         number of counters (read-only)
//   0xFF4         PERF_CMD (write-only, reads 0):
//                   bit 0 SNAPSHOT  copy every counter into the snapshot view at once
//                   bit 1 CLEAR     reset every live counter to 0
//                 Both bits in one write: the snapshot takes the values including
//                 this cycle's events, then the live counters restart from 0.
//
// A counter adds 1 in each cycle its event is set while enable_i (CTRL.run) is high.
// All other writes, and reads outside the map, return an error.
module counter_bank
  import tile_pkg::*;
#(
  parameter int unsigned N = tile_pkg::PC_NUM
) (
  input  logic          clk_i,
  input  logic          rst_ni,
  input  logic          enable_i,
  input  logic [N-1:0]  events_i,
  input  reg_req_t      core_req_i,
  output reg_rsp_t      core_rsp_o,
  input  reg_req_t      rmt_req_i,
  output reg_rsp_t      rmt_rsp_o
);
  initial assert (N * 8 <= int'(PERF_SNAP_BASE)) else $fatal(1, "too many counters");

  localparam int unsigned IW = $clog2(N);
  logic [63:0] cnt_q  [N];
  logic [63:0] snap_q [N];
  logic [31:0] hi_shadow_q [2];

  reg_req_t req [2];
  assign req[0] = core_req_i;
  assign req[1] = rmt_req_i;

  // Address decode per port
  logic [1:0] is_live, is_snap, is_num, is_cmd;
  always_comb
    for (int p = 0; p < 2; p++) begin
      is_live[p] = req[p].addr < 12'(8 * N);
      is_snap[p] = req[p].addr >= PERF_SNAP_BASE && req[p].addr < PERF_SNAP_BASE + 12'(8 * N);
      is_num[p]  = req[p].addr == PERF_NUM_OFFSET;
      is_cmd[p]  = req[p].addr == PERF_CMD_OFFSET;
    end

  logic [31:0] rdata [2];
  logic        err   [2];
  always_comb
    for (int p = 0; p < 2; p++) begin
      rdata[p] = '0;
      err[p]   = 1'b0;
      if (req[p].we) begin
        err[p] = !is_cmd[p];
      end else if (is_live[p]) begin
        rdata[p] = req[p].addr[2] ? hi_shadow_q[p] : cnt_q[req[p].addr[3 +: IW]][31:0];
      end else if (is_snap[p]) begin
        rdata[p] = req[p].addr[2] ? snap_q[req[p].addr[3 +: IW]][63:32]
                                  : snap_q[req[p].addr[3 +: IW]][31:0];
      end else if (is_num[p]) begin
        rdata[p] = 32'(N);
      end else if (!is_cmd[p]) begin
        err[p] = 1'b1;
      end
    end

  // Commands from either port (same cycle: OR of both)
  logic do_snap, do_clear;
  always_comb begin
    do_snap  = 1'b0;
    do_clear = 1'b0;
    for (int p = 0; p < 2; p++)
      if (req[p].req && req[p].we && is_cmd[p] && req[p].be[0]) begin
        do_snap  |= req[p].wdata[PERF_CMD_SNAPSHOT];
        do_clear |= req[p].wdata[PERF_CMD_CLEAR];
      end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      cnt_q       <= '{default: '0};
      snap_q      <= '{default: '0};
      hi_shadow_q <= '{default: '0};
    end else begin
      for (int i = 0; i < N; i++) begin
        logic [63:0] next;
        next = cnt_q[i] + 64'(enable_i && events_i[i]);
        if (do_snap)  snap_q[i] <= next;
        cnt_q[i] <= do_clear ? '0 : next;
      end
      for (int p = 0; p < 2; p++)
        if (req[p].req && !req[p].we && is_live[p] && !req[p].addr[2])
          hi_shadow_q[p] <= cnt_q[req[p].addr[3 +: IW]][63:32];
    end
  end

  reg_rsp_stage u_core_rsp (.clk_i, .rst_ni, .req_i(core_req_i.req), .err_i(err[0]), .rdata_i(rdata[0]), .rsp_o(core_rsp_o));
  reg_rsp_stage u_rmt_rsp  (.clk_i, .rst_ni, .req_i(rmt_req_i.req),  .err_i(err[1]), .rdata_i(rdata[1]), .rsp_o(rmt_rsp_o));

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{core_req_i.wdata, rmt_req_i.wdata, core_req_i.be, rmt_req_i.be, core_req_i.addr[1:0], rmt_req_i.addr[1:0]};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
