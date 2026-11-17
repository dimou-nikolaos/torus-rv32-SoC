// I/O tile alone: the testbench is the host on the AXI port (C++ bus functional
// model) and the rest of the grid on the NoC port (C++ tile model).
module tb_io_top
  import noc_pkg::*;
#(
  parameter int unsigned GX    = 4,
  parameter int unsigned GY    = 4,
  parameter int unsigned DEPTH = 5,
  parameter int unsigned OUT   = 8
) (
  input  logic          aclk_i,
  input  logic          arst_ni,
  input  logic [3:0]    s_axi_awid_i,
  input  logic [31:0]   s_axi_awaddr_i,
  input  logic [7:0]    s_axi_awlen_i,
  input  logic [2:0]    s_axi_awsize_i,
  input  logic [1:0]    s_axi_awburst_i,
  input  logic          s_axi_awvalid_i,
  output logic          s_axi_awready_o,
  input  logic [127:0]  s_axi_wdata_i,
  input  logic [15:0]   s_axi_wstrb_i,
  input  logic          s_axi_wlast_i,
  input  logic          s_axi_wvalid_i,
  output logic          s_axi_wready_o,
  output logic [3:0]    s_axi_bid_o,
  output logic [1:0]    s_axi_bresp_o,
  output logic          s_axi_bvalid_o,
  input  logic          s_axi_bready_i,
  input  logic [3:0]    s_axi_arid_i,
  input  logic [31:0]   s_axi_araddr_i,
  input  logic [7:0]    s_axi_arlen_i,
  input  logic [2:0]    s_axi_arsize_i,
  input  logic [1:0]    s_axi_arburst_i,
  input  logic          s_axi_arvalid_i,
  output logic          s_axi_arready_o,
  output logic [3:0]    s_axi_rid_o,
  output logic [127:0]  s_axi_rdata_o,
  output logic [1:0]    s_axi_rresp_o,
  output logic          s_axi_rlast_o,
  output logic          s_axi_rvalid_o,
  input  logic          s_axi_rready_i,
  input  logic          clk_i,
  input  logic          rst_ni,
  output logic          inj_valid_o,
  output logic [1:0]    inj_la_o,
  output flit_t         inj_flit_o,
  input  logic          inj_credit_i,
  input  logic          ej_valid_i,
  input  flit_t         ej_flit_i,
  output logic          ej_credit_o
);
  link_t inj, ej;
  assign ej = '{valid: ej_valid_i, la_port: P_LOCAL, flit: ej_flit_i};

  io_tile #(.GX(GX), .GY(GY), .DEPTH(DEPTH), .OUT(OUT), .IDW(4)) u_dut (
    .aclk_i, .arst_ni,
    .s_axi_awid_i, .s_axi_awaddr_i, .s_axi_awlen_i, .s_axi_awsize_i, .s_axi_awburst_i, .s_axi_awvalid_i,
    .s_axi_awready_o, .s_axi_wdata_i, .s_axi_wstrb_i, .s_axi_wlast_i, .s_axi_wvalid_i, .s_axi_wready_o,
    .s_axi_bid_o, .s_axi_bresp_o, .s_axi_bvalid_o, .s_axi_bready_i,
    .s_axi_arid_i, .s_axi_araddr_i, .s_axi_arlen_i, .s_axi_arsize_i, .s_axi_arburst_i, .s_axi_arvalid_i,
    .s_axi_arready_o, .s_axi_rid_o, .s_axi_rdata_o, .s_axi_rresp_o, .s_axi_rlast_o, .s_axi_rvalid_o,
    .s_axi_rready_i,
    .clk_i, .rst_ni, .inj_o(inj), .inj_credit_i, .ej_i(ej), .ej_credit_o
  );
  assign inj_valid_o = inj.valid;
  assign inj_la_o    = inj.la_port;
  assign inj_flit_o  = inj.flit;
endmodule
