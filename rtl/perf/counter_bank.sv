// Tile performance counters (SPEC.md, Tile memory map and NIU registers).
//
// NFIX fixed counters (cycles, retired instructions) and NPROG programmable
// counters, each counting the event its select register names. Counters are
// CW bits wide (48: days at GHz rates) and count while enable_i (CTRL.run).
//
//   0x000 + 8*i   live counter i: lo, then hi (hi latched when lo is read on the
//                 same port, so lo-then-hi is one consistent value)
//   0x400 + 4*k   event select of programmable counter k (counter NFIX + k)
//   0x800 + 8*i   snapshot of counter i: lo, hi (static until the next snapshot)
//   0xFF0         number of counters (read-only)
//   0xFF4         PERF_CMD (write-only, reads 0): bit 0 SNAPSHOT copies every
//                 counter into the snapshot view in the same cycle, bit 1 CLEAR
//                 restarts the live counters from 0; both in one write capture
//                 the totals, then restart
//   0xFF8         number of selectable events (read-only)
//
// Events and enable_i are registered before counting (the core's events come
// late in the cycle, behind the scratchpad grant): a counter counts an event
// one cycle after it happens, and a snapshot or clear sees the events up to the
// previous cycle.
//
// Changing a select does not clear its counter. A select naming no event counts
// nothing. Writes elsewhere, and reads outside the map, return an error.
module counter_bank
  import tile_pkg::*;
#(
  parameter int unsigned NEV   = EV_NUM,      // selectable events
  parameter int unsigned NFIX  = 2,           // counters 0..NFIX-1 count events 0..NFIX-1
  parameter int unsigned NPROG = 8,
  parameter int unsigned CW    = 48,
  parameter logic [NPROG-1:0][7:0] DEFAULT_SEL = '0
) (
  input  logic           clk_i,
  input  logic           rst_ni,
  input  logic           enable_i,
  input  logic [NEV-1:0] events_i,
  input  reg_req_t       core_req_i,
  output reg_rsp_t       core_rsp_o,
  input  reg_req_t       rmt_req_i,
  output reg_rsp_t       rmt_rsp_o
);
  localparam int unsigned N  = NFIX + NPROG;
  localparam int unsigned IW = $clog2(N);
  localparam int unsigned KW = (NPROG > 1) ? $clog2(NPROG) : 1;
  initial assert (N * 8 <= 12'h400 && CW > 32 && CW <= 64 && NEV <= 256)
    else $fatal(1, "counter_bank: bad parameters");

  logic [CW-1:0] cnt_q  [N];
  logic [CW-1:0] snap_q [N];
  logic [7:0]    sel_q  [NPROG];
  logic [31:0]   hi_shadow_q [2];

  reg_req_t req [2];
  assign req[0] = core_req_i;
  assign req[1] = rmt_req_i;

  // Address decode per port
  logic [1:0] is_live, is_sel, is_snap, is_num, is_cmd, is_nev;
  always_comb
    for (int p = 0; p < 2; p++) begin
      is_live[p] = req[p].addr < 12'(8 * N);
      is_sel[p]  = req[p].addr >= 12'h400 && req[p].addr < 12'h400 + 12'(4 * NPROG) && req[p].addr[1:0] == '0;
      is_snap[p] = req[p].addr >= PERF_SNAP_BASE && req[p].addr < PERF_SNAP_BASE + 12'(8 * N);
      is_num[p]  = req[p].addr == PERF_NUM_OFFSET;
      is_cmd[p]  = req[p].addr == PERF_CMD_OFFSET;
      is_nev[p]  = req[p].addr == PERF_NEV_OFFSET;
    end

  function automatic logic [31:0] hi_word(logic [CW-1:0] v);
    return 32'(v >> 32);
  endfunction

  logic [31:0] rdata [2];
  logic        err   [2];
  always_comb
    for (int p = 0; p < 2; p++) begin
      rdata[p] = '0;
      err[p]   = 1'b0;
      if (req[p].we) begin
        err[p] = !(is_cmd[p] || is_sel[p]);
      end else if (is_live[p]) begin
        rdata[p] = req[p].addr[2] ? hi_shadow_q[p] : cnt_q[req[p].addr[3 +: IW]][31:0];
      end else if (is_sel[p]) begin
        rdata[p] = {24'h0, sel_q[req[p].addr[2 +: KW]]};
      end else if (is_snap[p]) begin
        rdata[p] = req[p].addr[2] ? hi_word(snap_q[req[p].addr[3 +: IW]]) : snap_q[req[p].addr[3 +: IW]][31:0];
      end else if (is_num[p]) begin
        rdata[p] = 32'(N);
      end else if (is_nev[p]) begin
        rdata[p] = 32'(NEV);
      end else if (!is_cmd[p]) begin
        err[p] = 1'b1;
      end
    end

  // Commands and select writes from either port (remote wins on the same select)
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

  // The event each counter counts: registered events of the previous cycle
  logic [NEV-1:0] events_q;
  logic           enable_q;
  logic [N-1:0]   hit;
  logic [255:0]   ev_all;   // every select value names a bit; unused ones are 0
  assign ev_all = 256'(events_q);
  always_comb begin
    for (int i = 0; i < NFIX; i++) hit[i] = events_q[i];
    for (int k = 0; k < NPROG; k++) hit[NFIX + k] = ev_all[sel_q[k]];
  end

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      events_q <= '0;
      enable_q <= 1'b0;
    end else begin
      events_q <= events_i;
      enable_q <= enable_i;
    end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      cnt_q       <= '{default: '0};
      snap_q      <= '{default: '0};
      hi_shadow_q <= '{default: '0};
      for (int k = 0; k < NPROG; k++) sel_q[k] <= DEFAULT_SEL[k];
    end else begin
      for (int i = 0; i < N; i++) begin
        logic [CW-1:0] next;
        next = cnt_q[i] + CW'(enable_q && hit[i]);
        if (do_snap)  snap_q[i] <= next;
        cnt_q[i] <= do_clear ? '0 : next;
      end
      for (int p = 0; p < 2; p++) begin
        if (req[p].req && !req[p].we && is_live[p] && !req[p].addr[2])
          hi_shadow_q[p] <= hi_word(cnt_q[req[p].addr[3 +: IW]]);
        if (req[p].req && req[p].we && is_sel[p] && req[p].be[0])
          sel_q[req[p].addr[2 +: KW]] <= req[p].wdata[7:0];
      end
    end
  end

  reg_rsp_stage u_core_rsp (.clk_i, .rst_ni, .req_i(core_req_i.req), .err_i(err[0]), .rdata_i(rdata[0]), .rsp_o(core_rsp_o));
  reg_rsp_stage u_rmt_rsp  (.clk_i, .rst_ni, .req_i(rmt_req_i.req),  .err_i(err[1]), .rdata_i(rdata[1]), .rsp_o(rmt_rsp_o));

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{core_req_i.wdata, rmt_req_i.wdata, core_req_i.be, rmt_req_i.be, core_req_i.addr[1:0], rmt_req_i.addr[1:0]};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
