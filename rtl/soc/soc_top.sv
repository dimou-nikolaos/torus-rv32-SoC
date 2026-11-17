// Grid generator (SPEC.md, Baseline architecture): an NX x NY torus of compute
// tiles plus the I/O row, on NoC0. The router grid is GX x GY with GX = NX and
// GY = NY + 1; row y = NY is the I/O row, part of every Y ring. Each node's X
// ring output feeds the next node east, its Y ring output the next node south,
// wrapping around, with LINK_SLICE register slices on every link.
//
// The I/O row holds the I/O tile at (0, NY), whose AXI slave port is the
// design's boundary (in its own clock domain), and router-only stops (router,
// local port tied off) at the other positions.
module soc_top
  import noc_pkg::*;
#(
  parameter int unsigned NX         = 2,
  parameter int unsigned NY         = 2,
  parameter int unsigned LINK_SLICE = 0,
  parameter int unsigned IO_RD_OUTSTANDING = 8,
  parameter int unsigned IDW        = 4,          // AXI ID width
  localparam int unsigned GX        = NX,
  localparam int unsigned GY        = NY + 1,
  localparam int unsigned G         = GX * GY    // router node g = y * GX + x
) (
  input  logic           clk_i,
  input  logic           rst_ni,
  // AXI slave of the I/O tile, in the AXI clock domain
  input  logic           aclk_i,
  input  logic           arst_ni,
  input  logic [IDW-1:0] s_axi_awid_i,
  input  logic [31:0]    s_axi_awaddr_i,
  input  logic [7:0]     s_axi_awlen_i,
  input  logic [2:0]     s_axi_awsize_i,
  input  logic [1:0]     s_axi_awburst_i,
  input  logic           s_axi_awvalid_i,
  output logic           s_axi_awready_o,
  input  logic [127:0]   s_axi_wdata_i,
  input  logic [15:0]    s_axi_wstrb_i,
  input  logic           s_axi_wlast_i,
  input  logic           s_axi_wvalid_i,
  output logic           s_axi_wready_o,
  output logic [IDW-1:0] s_axi_bid_o,
  output logic [1:0]     s_axi_bresp_o,
  output logic           s_axi_bvalid_o,
  input  logic           s_axi_bready_i,
  input  logic [IDW-1:0] s_axi_arid_i,
  input  logic [31:0]    s_axi_araddr_i,
  input  logic [7:0]     s_axi_arlen_i,
  input  logic [2:0]     s_axi_arsize_i,
  input  logic [1:0]     s_axi_arburst_i,
  input  logic           s_axi_arvalid_i,
  output logic           s_axi_arready_o,
  output logic [IDW-1:0] s_axi_rid_o,
  output logic [127:0]   s_axi_rdata_o,
  output logic [1:0]     s_axi_rresp_o,
  output logic           s_axi_rlast_o,
  output logic           s_axi_rvalid_o,
  input  logic           s_axi_rready_i
);
  if (GX > 16 || GY > 16) begin : g_size_check
    $fatal(1, "soc_top: router grid %0dx%0d exceeds the 4-bit coordinates", GX, GY);
  end

  localparam int unsigned DEPTH = credit_rtt(LINK_SLICE) + 1;
  localparam int unsigned IO_X  = 0;

  link_t x_out [G], y_out [G], x_in [G], y_in [G];
  logic  x_out_cr [G], y_out_cr [G], x_in_cr [G], y_in_cr [G];

  for (genvar y = 0; y < GY; y++) begin : g_y
    for (genvar x = 0; x < GX; x++) begin : g_x
      localparam int unsigned g  = y * GX + x;
      localparam int unsigned XP = y * GX + (x + GX - 1) % GX;   // node feeding our X input
      localparam int unsigned YP = ((y + GY - 1) % GY) * GX + x; // node feeding our Y input

      if (y < NY) begin : g_tile
        tile_top #(.NX(NX), .NY(NY), .LINK_SLICE(LINK_SLICE), .IO_RD_OUTSTANDING(IO_RD_OUTSTANDING)) u_tile (
          .clk_i, .rst_ni, .coord_x_i(4'(x)), .coord_y_i(4'(y)),
          .x_in_i(x_in[g]), .x_in_credit_o(x_in_cr[g]), .x_out_o(x_out[g]), .x_out_credit_i(x_out_cr[g]),
          .y_in_i(y_in[g]), .y_in_credit_o(y_in_cr[g]), .y_out_o(y_out[g]), .y_out_credit_i(y_out_cr[g])
        );
      end else begin : g_io
        // I/O row: the I/O tile's router, or a router-only stop
        localparam bit IO = x == IO_X;
        link_t r_in [NPORTS], r_out [NPORTS];
        logic  r_cr_out [NPORTS], r_cr_in [NPORTS];
        logic [NPORTS-1:0] ev_flit, ev_cstall, ev_bhold, ev_bubble, ev_arbloss;
        link_t inj;
        logic  ej_credit;
        assign r_in[IL]    = inj;
        assign r_in[IX]    = x_in[g];
        assign r_in[IY]    = y_in[g];
        assign x_in_cr[g]  = r_cr_out[IX];
        assign y_in_cr[g]  = r_cr_out[IY];
        assign x_out[g]    = r_out[IX];
        assign y_out[g]    = r_out[IY];
        assign r_cr_in[IL] = ej_credit;
        assign r_cr_in[IX] = x_out_cr[g];
        assign r_cr_in[IY] = y_out_cr[g];
        noc_router #(.DIR(1'b0), .GX(GX), .GY(GY), .DEPTH(DEPTH), .LINK_SLICE(LINK_SLICE),
                     .HAS_LOCAL(IO)) u_router (
          .clk_i, .rst_ni, .x_i(coord_t'(x)), .y_i(coord_t'(y)),
          .in_i(r_in), .credit_o(r_cr_out), .out_o(r_out), .credit_i(r_cr_in),
          .ev_flit_o(ev_flit), .ev_credit_stall_o(ev_cstall), .ev_bubble_hold_o(ev_bhold),
          .ev_bubble_o(ev_bubble), .ev_arb_loss_o(ev_arbloss)
        );
        if (IO) begin : g_io_tile
          io_tile #(.GX(GX), .GY(GY), .IO_X(IO_X), .IO_Y(NY), .DEPTH(DEPTH), .OUT(IO_RD_OUTSTANDING),
                    .IDW(IDW)) u_io (
            .aclk_i, .arst_ni,
            .s_axi_awid_i, .s_axi_awaddr_i, .s_axi_awlen_i, .s_axi_awsize_i, .s_axi_awburst_i,
            .s_axi_awvalid_i, .s_axi_awready_o, .s_axi_wdata_i, .s_axi_wstrb_i, .s_axi_wlast_i,
            .s_axi_wvalid_i, .s_axi_wready_o, .s_axi_bid_o, .s_axi_bresp_o, .s_axi_bvalid_o,
            .s_axi_bready_i, .s_axi_arid_i, .s_axi_araddr_i, .s_axi_arlen_i, .s_axi_arsize_i,
            .s_axi_arburst_i, .s_axi_arvalid_i, .s_axi_arready_o, .s_axi_rid_o, .s_axi_rdata_o,
            .s_axi_rresp_o, .s_axi_rlast_o, .s_axi_rvalid_o, .s_axi_rready_i,
            .clk_i, .rst_ni, .inj_o(inj), .inj_credit_i(r_cr_out[IL]), .ej_i(r_out[IL]),
            .ej_credit_o(ej_credit)
          );
        end else begin : g_stop
          assign inj       = '0;
          assign ej_credit = 1'b0;
          /* verilator lint_off UNUSEDSIGNAL */
          logic unused_local;
          assign unused_local = ^{r_out[IL], r_cr_out[IL]};
          /* verilator lint_on UNUSEDSIGNAL */
        end
        /* verilator lint_off UNUSEDSIGNAL */
        logic unused;
        assign unused = ^{ev_flit, ev_cstall, ev_bhold, ev_bubble, ev_arbloss};
        /* verilator lint_on UNUSEDSIGNAL */
      end

      link_slice #(.DEPTH(LINK_SLICE)) u_xlink (
        .clk_i, .rst_ni, .fwd_i(x_out[XP]), .fwd_o(x_in[g]), .credit_i(x_in_cr[g]), .credit_o(x_out_cr[XP])
      );
      link_slice #(.DEPTH(LINK_SLICE)) u_ylink (
        .clk_i, .rst_ni, .fwd_i(y_out[YP]), .fwd_o(y_in[g]), .credit_i(y_in_cr[g]), .credit_o(y_out_cr[YP])
      );
    end
  end
endmodule
