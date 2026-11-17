// Compute tile, M0 subset: core, scratchpad with the full seven-port arbiter,
// tile control, counter bank and console. NIU and engine ports of the arbiter
// are present and tied off until M1 and M5.
//
// The remote register port (rmt_*) reaches tile control and the counter bank;
// the testbench drives it at M0 and the NIU from M1. Its address is a local
// byte address in the MMIO range.
module tile_top
  import tile_pkg::*;
#(
  parameter int unsigned STARVE_LIMIT = 4
) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [3:0]  coord_x_i,
  input  logic [3:0]  coord_y_i,

  input  logic        rmt_req_i,
  input  logic        rmt_we_i,
  input  logic [31:0] rmt_addr_i,
  input  logic [31:0] rmt_wdata_i,
  output logic        rmt_rvalid_o,
  output logic        rmt_err_o,
  output logic [31:0] rmt_rdata_o
);
  // ------------------------------------------------------------------ core
  logic        run;
  logic        instr_req, instr_gnt, instr_rvalid, instr_err;
  logic [31:0] instr_addr, instr_rdata;
  logic        data_req, data_gnt, data_rvalid, data_err, data_we;
  logic [31:0] data_addr, data_wdata, data_rdata;
  logic [3:0]  data_be;
  logic        ev_retire, ev_fetch_stall, ev_data_stall;
  logic        core_rst_n;
  logic        core_sleep;

  // CTRL.run low holds the core in reset (SPEC.md, launch protocol)
  assign core_rst_n = rst_ni && run;

  core_wrap u_core (
    .clk_i, .rst_ni(core_rst_n), .fetch_enable_i(1'b1),
    .hartid_i       ({24'h0, coord_y_i, coord_x_i}),
    .instr_req_o    (instr_req),   .instr_gnt_i   (instr_gnt),   .instr_rvalid_i(instr_rvalid),
    .instr_addr_o   (instr_addr),  .instr_rdata_i (instr_rdata), .instr_err_i   (instr_err),
    .data_req_o     (data_req),    .data_gnt_i    (data_gnt),    .data_rvalid_i (data_rvalid),
    .data_addr_o    (data_addr),   .data_be_o     (data_be),     .data_we_o     (data_we),
    .data_wdata_o   (data_wdata),  .data_rdata_i  (data_rdata),  .data_err_i    (data_err),
    .core_sleep_o   (core_sleep),
    .ev_retire_o    (ev_retire), .ev_fetch_stall_o(ev_fetch_stall), .ev_data_stall_o(ev_data_stall)
  );

  // ------------------------------------------------------------ scratchpad
  spm_req_t spm_req [SPM_PORTS];
  spm_rsp_t spm_rsp [SPM_PORTS];
  logic [SPM_PORTS-1:0] spm_denied, spm_promoted;

  for (genvar p = 0; p < SPM_PORTS; p++) begin : g_unused_port
    if (p != PORT_CORE_I && p != PORT_CORE_D) begin : g_tie
      assign spm_req[p] = '0;   // NIU (M1) and engine (M5)
    end
  end

  spm_banks #(.STARVE_LIMIT(STARVE_LIMIT)) u_spm (
    .clk_i, .rst_ni, .req_i(spm_req), .rsp_o(spm_rsp),
    .denied_o(spm_denied), .promoted_o(spm_promoted)
  );

  // Instruction port: scratchpad only; anything else is a bus error.
  logic ifetch_spm, ifetch_err_q, ispm_gnt, ispm_rvalid;
  assign ifetch_spm = instr_addr[31:SPM_AW] == '0;

  obi_spm_adapter u_ibus (
    .clk_i, .rst_ni,
    .req_i(instr_req && ifetch_spm), .gnt_o(ispm_gnt), .addr_i(instr_addr), .we_i(1'b0),
    .be_i(4'hF), .wdata_i('0), .rvalid_o(ispm_rvalid), .rdata_o(instr_rdata),
    .spm_req_o(spm_req[PORT_CORE_I]), .spm_rsp_i(spm_rsp[PORT_CORE_I])
  );
  assign instr_gnt    = ifetch_spm ? ispm_gnt : instr_req;
  assign instr_rvalid = ispm_rvalid || ifetch_err_q;
  assign instr_err    = ifetch_err_q;

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) ifetch_err_q <= 1'b0;
    else         ifetch_err_q <= instr_req && !ifetch_spm;

  // Data port: scratchpad, MMIO regions, or bus error. Every target answers
  // exactly one cycle after its grant, so responses stay in OBI order.
  logic d_spm, d_mmio, d_bad;
  mmio_region_e d_region;
  assign d_spm    = data_addr[31:SPM_AW] == '0;
  assign d_mmio   = data_addr[31:16] == MMIO_HI && data_addr[15:14] == 2'b00;  // regions 0..3
  assign d_region = mmio_region_e'(data_addr[15:12]);
  assign d_bad    = !d_spm && !d_mmio;

  logic        dspm_gnt, dspm_rvalid;
  logic [31:0] dspm_rdata;
  obi_spm_adapter u_dbus (
    .clk_i, .rst_ni,
    .req_i(data_req && d_spm), .gnt_o(dspm_gnt), .addr_i(data_addr), .we_i(data_we),
    .be_i(data_be), .wdata_i(data_wdata), .rvalid_o(dspm_rvalid), .rdata_o(dspm_rdata),
    .spm_req_o(spm_req[PORT_CORE_D]), .spm_rsp_i(spm_rsp[PORT_CORE_D])
  );

  reg_req_t tctrl_req, perf_req, console_req;
  reg_rsp_t tctrl_rsp, perf_rsp, console_rsp;
  function automatic reg_req_t mmio_sel(mmio_region_e r);
    return '{req: data_req && d_mmio && d_region == r, we: data_we,
             addr: data_addr[11:0], be: data_be, wdata: data_wdata};
  endfunction
  assign tctrl_req   = mmio_sel(MMIO_TCTRL);
  assign perf_req    = mmio_sel(MMIO_PERF);
  assign console_req = mmio_sel(MMIO_CONSOLE);

  logic bad_q;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) bad_q <= 1'b0;
    else         bad_q <= data_req && (d_bad || d_mmio && d_region == MMIO_NIU);
  // NIU registers arrive in M1; until then that region answers with an error.

  assign data_gnt    = d_spm ? dspm_gnt : data_req;
  assign data_rvalid = dspm_rvalid || bad_q || tctrl_rsp.rvalid || perf_rsp.rvalid || console_rsp.rvalid;
  assign data_err    = bad_q || tctrl_rsp.err || perf_rsp.err || console_rsp.err;
  assign data_rdata  = dspm_rvalid ? dspm_rdata : tctrl_rsp.rdata | perf_rsp.rdata;

  // ----------------------------------------------------------- remote port
  reg_req_t rmt_tctrl_req, rmt_perf_req;
  reg_rsp_t rmt_tctrl_rsp, rmt_perf_rsp;
  logic     rmt_hit_tctrl, rmt_hit_perf, rmt_bad_q;
  assign rmt_hit_tctrl = rmt_addr_i[31:12] == TCTRL_BASE[31:12];
  assign rmt_hit_perf  = rmt_addr_i[31:12] == PERF_BASE[31:12];
  always_comb begin
    rmt_tctrl_req       = '{req: rmt_req_i && rmt_hit_tctrl, we: rmt_we_i, addr: rmt_addr_i[11:0], be: 4'hF, wdata: rmt_wdata_i};
    rmt_perf_req        = rmt_tctrl_req;
    rmt_perf_req.req    = rmt_req_i && rmt_hit_perf;
  end
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) rmt_bad_q <= 1'b0;
    else         rmt_bad_q <= rmt_req_i && !rmt_hit_tctrl && !rmt_hit_perf;

  assign rmt_rvalid_o = rmt_tctrl_rsp.rvalid || rmt_perf_rsp.rvalid || rmt_bad_q;
  assign rmt_err_o    = rmt_tctrl_rsp.err || rmt_perf_rsp.err || rmt_bad_q;
  assign rmt_rdata_o  = rmt_tctrl_rsp.rdata | rmt_perf_rsp.rdata;

  // ------------------------------------------------------- register blocks
  logic [63:0] cycle;
  tile_ctrl u_tctrl (
    .clk_i, .rst_ni, .coord_x_i, .coord_y_i,
    .core_req_i(tctrl_req), .core_rsp_o(tctrl_rsp),
    .rmt_req_i (rmt_tctrl_req),        .rmt_rsp_o (rmt_tctrl_rsp),
    .doorbell_set_i(1'b0), .run_o(run), .cycle_o(cycle)
  );

  logic [PC_NUM-1:0] perf_ev;
  always_comb begin
    perf_ev                 = '0;
    perf_ev[PC_CYCLES]      = 1'b1;
    perf_ev[PC_RETIRED]     = ev_retire;
    perf_ev[PC_FETCH_STALL] = ev_fetch_stall;
    perf_ev[PC_DATA_STALL]  = ev_data_stall;
    perf_ev[PC_DENIED_0   +: SPM_PORTS] = spm_denied;
    perf_ev[PC_PROMOTED_0 +: SPM_PORTS] = spm_promoted;
  end

  counter_bank #(.N(PC_NUM)) u_perf (
    .clk_i, .rst_ni, .enable_i(run), .events_i(perf_ev),
    .core_req_i(perf_req), .core_rsp_o(perf_rsp),
    .rmt_req_i (rmt_perf_req),        .rmt_rsp_o (rmt_perf_rsp)
  );

  sim_console u_console (
    .clk_i, .rst_ni, .coord_x_i, .coord_y_i,
    .req_i(console_req), .rsp_o(console_rsp)
  );

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{cycle, core_sleep, console_rsp.rdata, data_addr[13:12] == 2'b00 && d_mmio};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
