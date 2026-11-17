// I/O tile (SPEC.md, I/O tile): AXI4 slave front-end in the AXI clock domain,
// two asynchronous FIFOs (requests in, read beats out; the design's only clock
// crossing), the AXI-to-NoC bridge with its network interface, and the
// register block with the host semaphores. Sits at (IO_X, IO_Y) in the I/O row
// and drives the local port of that position's router.
module io_tile
  import noc_pkg::*;
  import io_pkg::*;
#(
  parameter int unsigned GX       = 2,
  parameter int unsigned GY       = 3,
  parameter int unsigned IO_X     = 0,
  parameter int unsigned IO_Y     = GY - 1,
  parameter int unsigned DEPTH    = 5,    // router input buffers, and this ejection buffer
  parameter int unsigned OUT      = 8,    // IO_RD_OUTSTANDING: READ_REQs in flight
  parameter int unsigned IDW      = 4,    // AXI ID width
  parameter int unsigned CDC_DEPTH = 16
) (
  // AXI clock domain
  input  logic              aclk_i,
  input  logic              arst_ni,
  input  logic [IDW-1:0]    s_axi_awid_i,
  input  logic [31:0]       s_axi_awaddr_i,
  input  logic [7:0]        s_axi_awlen_i,
  input  logic [2:0]        s_axi_awsize_i,
  input  logic [1:0]        s_axi_awburst_i,
  input  logic              s_axi_awvalid_i,
  output logic              s_axi_awready_o,
  input  logic [127:0]      s_axi_wdata_i,
  input  logic [15:0]       s_axi_wstrb_i,
  input  logic              s_axi_wlast_i,
  input  logic              s_axi_wvalid_i,
  output logic              s_axi_wready_o,
  output logic [IDW-1:0]    s_axi_bid_o,
  output logic [1:0]        s_axi_bresp_o,
  output logic              s_axi_bvalid_o,
  input  logic              s_axi_bready_i,
  input  logic [IDW-1:0]    s_axi_arid_i,
  input  logic [31:0]       s_axi_araddr_i,
  input  logic [7:0]        s_axi_arlen_i,
  input  logic [2:0]        s_axi_arsize_i,
  input  logic [1:0]        s_axi_arburst_i,
  input  logic              s_axi_arvalid_i,
  output logic              s_axi_arready_o,
  output logic [IDW-1:0]    s_axi_rid_o,
  output logic [127:0]      s_axi_rdata_o,
  output logic [1:0]        s_axi_rresp_o,
  output logic              s_axi_rlast_o,
  output logic              s_axi_rvalid_o,
  input  logic              s_axi_rready_i,
  // Fabric clock domain
  input  logic              clk_i,
  input  logic              rst_ni,
  output link_t             inj_o,
  input  logic              inj_credit_i,
  input  link_t             ej_i,
  output logic              ej_credit_o
);
  io_req_t req_w, req_r;
  io_rsp_t rsp_w, rsp_r;
  logic    req_push, req_full, req_pop, req_empty;
  logic    rsp_push, rsp_full, rsp_pop, rsp_empty;

  io_axi_front #(.GX(GX), .GY(GY), .IO_X(IO_X), .IO_Y(IO_Y), .IDW(IDW)) u_front (
    .clk_i(aclk_i), .rst_ni(arst_ni),
    .awid_i(s_axi_awid_i), .awaddr_i(s_axi_awaddr_i), .awlen_i(s_axi_awlen_i), .awsize_i(s_axi_awsize_i),
    .awburst_i(s_axi_awburst_i), .awvalid_i(s_axi_awvalid_i), .awready_o(s_axi_awready_o),
    .wdata_i(s_axi_wdata_i), .wstrb_i(s_axi_wstrb_i), .wlast_i(s_axi_wlast_i), .wvalid_i(s_axi_wvalid_i),
    .wready_o(s_axi_wready_o),
    .bid_o(s_axi_bid_o), .bresp_o(s_axi_bresp_o), .bvalid_o(s_axi_bvalid_o), .bready_i(s_axi_bready_i),
    .arid_i(s_axi_arid_i), .araddr_i(s_axi_araddr_i), .arlen_i(s_axi_arlen_i), .arsize_i(s_axi_arsize_i),
    .arburst_i(s_axi_arburst_i), .arvalid_i(s_axi_arvalid_i), .arready_o(s_axi_arready_o),
    .rid_o(s_axi_rid_o), .rdata_o(s_axi_rdata_o), .rresp_o(s_axi_rresp_o), .rlast_o(s_axi_rlast_o),
    .rvalid_o(s_axi_rvalid_o), .rready_i(s_axi_rready_i),
    .req_push_o(req_push), .req_o(req_w), .req_full_i(req_full),
    .rsp_i(rsp_r), .rsp_empty_i(rsp_empty), .rsp_pop_o(rsp_pop)
  );

  io_cdc_fifo #(.WIDTH($bits(io_req_t)), .DEPTH(CDC_DEPTH)) u_req_cdc (
    .wclk_i(aclk_i), .wrst_ni(arst_ni), .push_i(req_push), .data_i(req_w), .full_o(req_full),
    .rclk_i(clk_i), .rrst_ni(rst_ni), .pop_i(req_pop), .head_o(req_r), .empty_o(req_empty)
  );
  io_cdc_fifo #(.WIDTH($bits(io_rsp_t)), .DEPTH(CDC_DEPTH)) u_rsp_cdc (
    .wclk_i(clk_i), .wrst_ni(rst_ni), .push_i(rsp_push), .data_i(rsp_w), .full_o(rsp_full),
    .rclk_i(aclk_i), .rrst_ni(arst_ni), .pop_i(rsp_pop), .head_o(rsp_r), .empty_o(rsp_empty)
  );

  logic         host_we, rx_we, rx_inc;
  logic [11:0]  host_addr, host_raddr, rx_addr, rx_inc_addr;
  logic [15:0]  host_strb, rx_be;
  logic [127:0] host_wdata, host_rdata, rx_wdata;
  logic [31:0]  rx_inc_val;
  logic         ev_wr, ev_rd, ev_rsp, ev_err;

  io_bridge #(.DEPTH(DEPTH), .OUT(OUT)) u_bridge (
    .clk_i, .rst_ni, .x_i(coord_t'(IO_X)), .y_i(coord_t'(IO_Y)),
    .req_i(req_r), .req_empty_i(req_empty), .req_pop_o(req_pop),
    .rsp_push_o(rsp_push), .rsp_o(rsp_w), .rsp_full_i(rsp_full),
    .inj_o, .inj_credit_i, .ej_i, .ej_credit_o,
    .host_we_o(host_we), .host_addr_o(host_addr), .host_strb_o(host_strb), .host_wdata_o(host_wdata),
    .host_raddr_o(host_raddr), .host_rdata_i(host_rdata),
    .rx_we_o(rx_we), .rx_addr_o(rx_addr), .rx_be_o(rx_be), .rx_wdata_o(rx_wdata),
    .rx_inc_o(rx_inc), .rx_inc_addr_o(rx_inc_addr), .rx_inc_val_o(rx_inc_val),
    .ev_wr_flit_o(ev_wr), .ev_rd_req_o(ev_rd), .ev_rsp_flit_o(ev_rsp), .ev_rx_err_o(ev_err)
  );

  io_regs #(.GX(GX), .GY(GY), .OUT(OUT)) u_regs (
    .clk_i, .rst_ni,
    .host_we_i(host_we), .host_addr_i(host_addr), .host_strb_i(host_strb), .host_wdata_i(host_wdata),
    .host_rdata_o(host_rdata), .host_raddr_i(host_raddr),
    .rx_we_i(rx_we), .rx_addr_i(rx_addr), .rx_be_i(rx_be), .rx_wdata_i(rx_wdata),
    .rx_inc_i(rx_inc), .rx_inc_addr_i(rx_inc_addr), .rx_inc_val_i(rx_inc_val),
    .ev_wr_flit_i(ev_wr), .ev_rd_req_i(ev_rd), .ev_rsp_flit_i(ev_rsp), .ev_rx_err_i(ev_err)
  );
endmodule
