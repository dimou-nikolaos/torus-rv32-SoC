// Compute tile, M1: core, scratchpad with the seven-port arbiter, tile control,
// counter bank, console, NIU and the NoC0 router. The engine port of the
// arbiter is tied off until M5; NoC1 arrives in M3.
//
// Remote register accesses to tile control and the counter bank come from the
// NIU (WRITE flits). Until the I/O tile exists (M2), the testbench also has a
// register port here (tb_*), used only while the NoC is quiet.
module tile_top
  import tile_pkg::*;
  import noc_pkg::*;
#(
  parameter int unsigned STARVE_LIMIT = 4,
  parameter int unsigned NX           = 1,    // compute grid; the router grid adds the I/O row
  parameter int unsigned NY           = 1,
  parameter int unsigned LINK_SLICE   = 0,
  localparam int unsigned GX          = NX,   // NoC ring lengths
  localparam int unsigned GY          = NY + 1,
  localparam int unsigned DEPTH       = credit_rtt(LINK_SLICE) + 1
) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [3:0]  coord_x_i,
  input  logic [3:0]  coord_y_i,

  // NoC0 ring ports: X ring and Y ring, in and out
  input  link_t       x_in_i,
  output logic        x_in_credit_o,
  output link_t       x_out_o,
  input  logic        x_out_credit_i,
  input  link_t       y_in_i,
  output logic        y_in_credit_o,
  output link_t       y_out_o,
  input  logic        y_out_credit_i,

  // Testbench register port (M1 scaffolding, removed in M2)
  input  logic        tb_req_i,
  input  logic        tb_we_i,
  input  logic [31:0] tb_addr_i,
  input  logic [31:0] tb_wdata_i,
  output logic        tb_rvalid_o,
  output logic        tb_err_o,
  output logic [31:0] tb_rdata_o
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

  // Engine (M5), NoC1 receive and the reply engine (M2/M3) are not connected yet
  assign spm_req[PORT_ENGINE] = '0;
  assign spm_req[PORT_RX1]    = '0;
  assign spm_req[PORT_TXREP]  = '0;

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

  reg_req_t tctrl_req, perf_req, console_req, niu_req;
  reg_rsp_t tctrl_rsp, perf_rsp, console_rsp, niu_rsp;
  function automatic reg_req_t mmio_sel(mmio_region_e r);
    return '{req: data_req && d_mmio && d_region == r, we: data_we,
             addr: data_addr[11:0], be: data_be, wdata: data_wdata};
  endfunction
  assign tctrl_req   = mmio_sel(MMIO_TCTRL);
  assign perf_req    = mmio_sel(MMIO_PERF);
  assign console_req = mmio_sel(MMIO_CONSOLE);
  assign niu_req     = mmio_sel(MMIO_NIU);

  logic bad_q;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) bad_q <= 1'b0;
    else         bad_q <= data_req && d_bad;

  assign data_gnt    = d_spm ? dspm_gnt : data_req;
  assign data_rvalid = dspm_rvalid || bad_q || tctrl_rsp.rvalid || perf_rsp.rvalid || console_rsp.rvalid
                     || niu_rsp.rvalid;
  assign data_err    = bad_q || tctrl_rsp.err || perf_rsp.err || console_rsp.err || niu_rsp.err;
  assign data_rdata  = dspm_rvalid ? dspm_rdata : tctrl_rsp.rdata | perf_rsp.rdata | niu_rsp.rdata;

  // ------------------------------------------------------------ NIU, router
  link_t inj, ej;
  logic  inj_credit, ej_credit;
  logic  niu_rmt_req;
  logic [31:0] niu_rmt_addr, niu_rmt_wdata;
  logic  ev_tx, ev_rx, ev_err;

  niu #(.GX(GX), .GY(GY), .IO_ROW(1'b1), .DEPTH(DEPTH)) u_niu (
    .clk_i, .rst_ni, .x_i(coord_t'(coord_x_i)), .y_i(coord_t'(coord_y_i)),
    .reg_req_i(niu_req), .reg_rsp_o(niu_rsp),
    .tx_spm_req_o(spm_req[PORT_TXDMA]), .tx_spm_rsp_i(spm_rsp[PORT_TXDMA]),
    .rx_spm_req_o(spm_req[PORT_RX0]),   .rx_spm_rsp_i(spm_rsp[PORT_RX0]),
    .rmt_req_o(niu_rmt_req), .rmt_addr_o(niu_rmt_addr), .rmt_wdata_o(niu_rmt_wdata),
    .inj_o(inj), .inj_credit_i(inj_credit), .ej_i(ej), .ej_credit_o(ej_credit),
    .ev_tx_flit_o(ev_tx), .ev_rx_flit_o(ev_rx), .ev_error_o(ev_err)
  );

  link_t r_in [NPORTS], r_out [NPORTS];
  logic  r_cr_out [NPORTS], r_cr_in [NPORTS];
  logic [NPORTS-1:0] r_ev_flit, r_ev_cstall, r_ev_bhold, r_ev_bubble, r_ev_arbloss;
  assign r_in[IL] = inj;
  assign r_in[IX] = x_in_i;
  assign r_in[IY] = y_in_i;
  assign inj_credit    = r_cr_out[IL];
  assign x_in_credit_o = r_cr_out[IX];
  assign y_in_credit_o = r_cr_out[IY];
  assign ej      = r_out[IL];
  assign x_out_o = r_out[IX];
  assign y_out_o = r_out[IY];
  assign r_cr_in[IL] = ej_credit;
  assign r_cr_in[IX] = x_out_credit_i;
  assign r_cr_in[IY] = y_out_credit_i;

  noc_router #(.DIR(1'b0), .GX(GX), .GY(GY), .DEPTH(DEPTH), .LINK_SLICE(LINK_SLICE)) u_r0 (
    .clk_i, .rst_ni, .x_i(coord_t'(coord_x_i)), .y_i(coord_t'(coord_y_i)),
    .in_i(r_in), .credit_o(r_cr_out), .out_o(r_out), .credit_i(r_cr_in),
    .ev_flit_o(r_ev_flit), .ev_credit_stall_o(r_ev_cstall), .ev_bubble_hold_o(r_ev_bhold),
    .ev_bubble_o(r_ev_bubble), .ev_arb_loss_o(r_ev_arbloss)
  );

  // ----------------------------------------------------------- remote port
  // Remote accesses from the NIU (write-only) and from the testbench (M1).
  logic        rmt_req, rmt_we;
  logic [31:0] rmt_addr, rmt_wdata;
  assign rmt_req   = niu_rmt_req || tb_req_i;
  assign rmt_we    = niu_rmt_req ? 1'b1 : tb_we_i;
  assign rmt_addr  = niu_rmt_req ? niu_rmt_addr : tb_addr_i;
  assign rmt_wdata = niu_rmt_req ? niu_rmt_wdata : tb_wdata_i;

  reg_req_t rmt_tctrl_req, rmt_perf_req;
  reg_rsp_t rmt_tctrl_rsp, rmt_perf_rsp;
  logic     rmt_hit_tctrl, rmt_hit_perf, rmt_bad_q, tb_q;
  assign rmt_hit_tctrl = rmt_addr[31:12] == TCTRL_BASE[31:12];
  assign rmt_hit_perf  = rmt_addr[31:12] == PERF_BASE[31:12];
  always_comb begin
    rmt_tctrl_req       = '{req: rmt_req && rmt_hit_tctrl, we: rmt_we, addr: rmt_addr[11:0], be: 4'hF, wdata: rmt_wdata};
    rmt_perf_req        = rmt_tctrl_req;
    rmt_perf_req.req    = rmt_req && rmt_hit_perf;
  end
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      rmt_bad_q <= 1'b0;
      tb_q      <= 1'b0;
    end else begin
      rmt_bad_q <= rmt_req && !rmt_hit_tctrl && !rmt_hit_perf;
      tb_q      <= tb_req_i && !niu_rmt_req;
    end

  // Responses go to the testbench only (NIU writes are posted)
  assign tb_rvalid_o = tb_q;
  assign tb_err_o    = tb_q && (rmt_tctrl_rsp.err || rmt_perf_rsp.err || rmt_bad_q);
  assign tb_rdata_o  = rmt_tctrl_rsp.rdata | rmt_perf_rsp.rdata;

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni) assert (!(tb_req_i && niu_rmt_req))
      else $error("tile_top (%0d,%0d): testbench register access collided with a NoC write", coord_x_i, coord_y_i);
`endif

  // ------------------------------------------------------- register blocks
  logic [63:0] cycle;
  tile_ctrl #(.NX(NX), .NY(NY), .IO_X(0), .IO_Y(NY)) u_tctrl (
    .clk_i, .rst_ni, .coord_x_i, .coord_y_i,
    .core_req_i(tctrl_req), .core_rsp_o(tctrl_rsp),
    .rmt_req_i (rmt_tctrl_req),        .rmt_rsp_o (rmt_tctrl_rsp),
    .doorbell_set_i(1'b0), .run_o(run), .cycle_o(cycle)
  );

  logic [EV_NUM-1:0] perf_ev;
  always_comb begin
    perf_ev                 = '0;
    perf_ev[EV_CYCLES]      = 1'b1;
    perf_ev[EV_RETIRED]     = ev_retire;
    perf_ev[EV_FETCH_STALL] = ev_fetch_stall;
    perf_ev[EV_DATA_STALL]  = ev_data_stall;
    perf_ev[EV_DENIED_0   +: SPM_PORTS] = spm_denied;
    perf_ev[EV_PROMOTED_0 +: SPM_PORTS] = spm_promoted;
    perf_ev[EV_NIU_TX]                  = ev_tx;
    perf_ev[EV_NIU_RX]                  = ev_rx;
    perf_ev[EV_NIU_ERR]                 = ev_err;
    perf_ev[EV_R0_FLIT    +: NPORTS]    = r_ev_flit;
    perf_ev[EV_R0_CSTALL  +: NPORTS]    = r_ev_cstall;
    perf_ev[EV_R0_ARBLOSS +: NPORTS]    = r_ev_arbloss;
    perf_ev[EV_R0_BHOLD   +: NPORTS]    = r_ev_bhold;
  end

  counter_bank #(.NEV(EV_NUM), .NFIX(PERF_NFIX), .NPROG(PERF_NPROG), .DEFAULT_SEL(PERF_DEFAULT_SEL)) u_perf (
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
  assign unused = ^{cycle, core_sleep, console_rsp.rdata, data_addr[13:12] == 2'b00 && d_mmio, r_ev_bubble,
                    rmt_tctrl_rsp.rvalid, rmt_perf_rsp.rvalid};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
