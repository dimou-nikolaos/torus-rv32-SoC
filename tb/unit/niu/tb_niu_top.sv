// NIU with its real scratchpad. The testbench drives the NIU registers, plays
// the network of both planes on both sides (takes injected flits and returns
// credits; sends ejected flits within the NIU's credits), models tile control
// and the counter bank on the remote register port, and adds random core
// traffic on the core ports to create bank conflicts. Per-plane signals are
// indexed by plane (0: NoC0, 1: NoC1).
module tb_niu_top
  import tile_pkg::*;
  import noc_pkg::*;
#(
  parameter int unsigned GX    = 4,
  parameter int unsigned GY    = 4,     // row GY-1 is the I/O row
  parameter int unsigned DEPTH = 5,
  parameter int unsigned RQ    = 6      // READ_REQs in flight grid-wide (queue depth)
) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [3:0]  x_i,
  input  logic [3:0]  y_i,
  // registers
  input  logic        reg_req_i,
  input  logic        reg_we_i,
  input  logic [11:0] reg_addr_i,
  input  logic [31:0] reg_wdata_i,
  output logic        reg_rvalid_o,
  output logic        reg_err_o,
  output logic [31:0] reg_rdata_o,
  // network side, per plane
  output logic [1:0]       inj_valid_o,
  output logic [1:0][1:0]  inj_la_o,
  output flit_t [1:0]      inj_flit_o,
  input  logic [1:0]       inj_credit_i,
  input  logic [1:0]       ej_valid_i,
  input  flit_t [1:0]      ej_flit_i,
  output logic [1:0]       ej_credit_o,
  // remote register port
  output logic        rmt_req_o,
  output logic        rmt_port_o,     // a register write's RX path (1: NoC1)
  output logic        rmt_we_o,
  output logic [31:0] rmt_addr_o,
  output logic [3:0]  rmt_be_o,
  output logic [31:0] rmt_wdata_o,
  input  logic        rmt_rvalid_i,
  input  logic        rmt_err_i,
  input  logic [31:0] rmt_rdata_i,
  output logic        doorbell_o,
  // RX scratchpad writes as granted, per plane (for the ordering check)
  output logic [1:0]       rx_wr_o,
  output logic [1:0]       rx_wr_add_o,
  output logic [1:0][15:0] rx_wr_addr_o,
  // contention from the core ports (reads)
  input  logic        cd_req_i,
  input  logic [15:0] cd_addr_i,
  input  logic        ci_req_i,
  input  logic [15:0] ci_addr_i,
  output logic [1:0]  ev_tx_o,
  output logic [1:0]  ev_rx_o,
  output logic        ev_err_o
);
  spm_req_t req [SPM_PORTS];
  spm_rsp_t rsp [SPM_PORTS];
  logic [SPM_PORTS-1:0] den, prom;
  reg_req_t rq;
  reg_rsp_t rs;
  link_t inj [2], ej [2];
  logic  inj_credit [2], ej_credit [2];

  assign rq = '{req: reg_req_i, we: reg_we_i, addr: reg_addr_i, be: 4'hF, wdata: reg_wdata_i};
  assign reg_rvalid_o = rs.rvalid;
  assign reg_err_o    = rs.err;
  assign reg_rdata_o  = rs.rdata;

  assign req[PORT_ENGINE] = '0;
  assign req[PORT_CORE_D] = '{req: cd_req_i, we: 1'b0, lock: 1'b0, add: 1'b0, addr: cd_addr_i, be: '1, wdata: '0};
  assign req[PORT_CORE_I] = '{req: ci_req_i, we: 1'b0, lock: 1'b0, add: 1'b0, addr: ci_addr_i, be: '1, wdata: '0};

  spm_banks u_spm (.clk_i, .rst_ni, .req_i(req), .rsp_o(rsp), .denied_o(den), .promoted_o(prom));

  for (genvar p = 0; p < 2; p++) begin : g_net
    localparam int unsigned RXP = p == 0 ? PORT_RX0 : PORT_RX1;
    assign ej[p]           = '{valid: ej_valid_i[p], la_port: P_LOCAL, flit: ej_flit_i[p]};
    assign inj_credit[p]   = inj_credit_i[p];
    assign ej_credit_o[p]  = ej_credit[p];
    assign inj_valid_o[p]  = inj[p].valid;
    assign inj_la_o[p]     = inj[p].la_port;
    assign inj_flit_o[p]   = inj[p].flit;
    assign rx_wr_o[p]      = req[RXP].req && req[RXP].we && rsp[RXP].gnt;
    assign rx_wr_add_o[p]  = req[RXP].add;
    assign rx_wr_addr_o[p] = req[RXP].addr;
  end

  niu #(.GX(GX), .GY(GY), .IO_ROW(1'b1), .DEPTH(DEPTH), .RQ_NEEDED(RQ)) u_dut (
    .clk_i, .rst_ni, .x_i(coord_t'(x_i)), .y_i(coord_t'(y_i)),
    .reg_req_i(rq), .reg_rsp_o(rs),
    .tx_spm_req_o (req[PORT_TXDMA]), .tx_spm_rsp_i (rsp[PORT_TXDMA]),
    .rep_spm_req_o(req[PORT_TXREP]), .rep_spm_rsp_i(rsp[PORT_TXREP]),
    .rx0_spm_req_o(req[PORT_RX0]),   .rx0_spm_rsp_i(rsp[PORT_RX0]),
    .rx1_spm_req_o(req[PORT_RX1]),   .rx1_spm_rsp_i(rsp[PORT_RX1]),
    .rmt_req_o, .rmt_we_o, .rmt_addr_o, .rmt_be_o, .rmt_wdata_o, .rmt_rvalid_i, .rmt_err_i, .rmt_rdata_i,
    .doorbell_o,
    .inj_o(inj), .inj_credit_i(inj_credit), .ej_i(ej), .ej_credit_o(ej_credit),
    .ev_tx_flit_o(ev_tx_o), .ev_rx_flit_o(ev_rx_o), .ev_error_o(ev_err_o)
  );
  assign rmt_port_o = u_dut.reg_gnt[1];

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{den, prom, rsp[PORT_CORE_D], rsp[PORT_CORE_I], rsp[PORT_ENGINE]};
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
