// NIU with its real scratchpad. The testbench drives the NIU registers, plays
// the network on both sides (takes injected flits and returns credits; sends
// ejected flits within the NIU's credits), models tile control and the counter
// bank on the remote register port, and adds random core traffic on the core
// ports to create bank conflicts.
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
  // network side
  output logic        inj_valid_o,
  output logic [1:0]  inj_la_o,
  output flit_t       inj_flit_o,
  input  logic        inj_credit_i,
  input  logic        ej_valid_i,
  input  flit_t       ej_flit_i,
  output logic        ej_credit_o,
  // remote register port
  output logic        rmt_req_o,
  output logic        rmt_we_o,
  output logic [31:0] rmt_addr_o,
  output logic [3:0]  rmt_be_o,
  output logic [31:0] rmt_wdata_o,
  input  logic        rmt_rvalid_i,
  input  logic        rmt_err_i,
  input  logic [31:0] rmt_rdata_i,
  output logic        doorbell_o,
  // RX scratchpad writes as granted (for the ordering check)
  output logic        rx_wr_o,
  output logic        rx_wr_add_o,
  output logic [15:0] rx_wr_addr_o,
  // contention from the core ports (reads)
  input  logic        cd_req_i,
  input  logic [15:0] cd_addr_i,
  input  logic        ci_req_i,
  input  logic [15:0] ci_addr_i,
  output logic        ev_tx_o,
  output logic        ev_rx_o,
  output logic        ev_err_o
);
  spm_req_t req [SPM_PORTS];
  spm_rsp_t rsp [SPM_PORTS];
  logic [SPM_PORTS-1:0] den, prom;
  reg_req_t rq;
  reg_rsp_t rs;
  link_t inj, ej;

  assign rq = '{req: reg_req_i, we: reg_we_i, addr: reg_addr_i, be: 4'hF, wdata: reg_wdata_i};
  assign reg_rvalid_o = rs.rvalid;
  assign reg_err_o    = rs.err;
  assign reg_rdata_o  = rs.rdata;

  assign req[PORT_ENGINE] = '0;
  assign req[PORT_RX1]    = '0;
  assign req[PORT_CORE_D] = '{req: cd_req_i, we: 1'b0, lock: 1'b0, add: 1'b0, addr: cd_addr_i, be: '1, wdata: '0};
  assign req[PORT_CORE_I] = '{req: ci_req_i, we: 1'b0, lock: 1'b0, add: 1'b0, addr: ci_addr_i, be: '1, wdata: '0};

  spm_banks u_spm (.clk_i, .rst_ni, .req_i(req), .rsp_o(rsp), .denied_o(den), .promoted_o(prom));

  assign ej = '{valid: ej_valid_i, la_port: P_LOCAL, flit: ej_flit_i};

  niu #(.GX(GX), .GY(GY), .IO_ROW(1'b1), .DEPTH(DEPTH), .RQ_NEEDED(RQ)) u_dut (
    .clk_i, .rst_ni, .x_i(coord_t'(x_i)), .y_i(coord_t'(y_i)),
    .reg_req_i(rq), .reg_rsp_o(rs),
    .tx_spm_req_o (req[PORT_TXDMA]), .tx_spm_rsp_i (rsp[PORT_TXDMA]),
    .rep_spm_req_o(req[PORT_TXREP]), .rep_spm_rsp_i(rsp[PORT_TXREP]),
    .rx_spm_req_o (req[PORT_RX0]),   .rx_spm_rsp_i (rsp[PORT_RX0]),
    .rmt_req_o, .rmt_we_o, .rmt_addr_o, .rmt_be_o, .rmt_wdata_o, .rmt_rvalid_i, .rmt_err_i, .rmt_rdata_i,
    .doorbell_o,
    .inj_o(inj), .inj_credit_i, .ej_i(ej), .ej_credit_o,
    .ev_tx_flit_o(ev_tx_o), .ev_rx_flit_o(ev_rx_o), .ev_error_o(ev_err_o)
  );
  assign inj_valid_o  = inj.valid;
  assign inj_la_o     = inj.la_port;
  assign inj_flit_o   = inj.flit;
  assign rx_wr_o      = req[PORT_RX0].req && req[PORT_RX0].we && rsp[PORT_RX0].gnt;
  assign rx_wr_add_o  = req[PORT_RX0].add;
  assign rx_wr_addr_o = req[PORT_RX0].addr;

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{den, prom, rsp[PORT_CORE_D], rsp[PORT_CORE_I], rsp[PORT_ENGINE], rsp[PORT_RX1]};
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
