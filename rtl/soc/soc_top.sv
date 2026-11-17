// Grid generator (SPEC.md, Baseline architecture): an NX x NY torus of compute
// tiles plus the I/O row, on NoC0. The router grid is GX x GY with GX = NX and
// GY = NY + 1; row y = NY is the I/O row, part of every Y ring. Each node's X
// ring output feeds the next node east, its Y ring output the next node south,
// wrapping around, with LINK_SLICE register slices on every link.
//
// The I/O row holds router-only stops (router, local port tied off); the I/O
// tile at (0, NY) arrives with the bridge. The per-tile testbench register
// ports (tb_*) are M1 scaffolding.
module soc_top
  import noc_pkg::*;
#(
  parameter int unsigned NX         = 2,
  parameter int unsigned NY         = 2,
  parameter int unsigned LINK_SLICE = 0,
  localparam int unsigned N         = NX * NY,   // compute tile n = y * NX + x
  localparam int unsigned GX        = NX,
  localparam int unsigned GY        = NY + 1,
  localparam int unsigned G         = GX * GY    // router node g = y * GX + x
) (
  input  logic              clk_i,
  input  logic              rst_ni,
  input  logic [N-1:0]      tb_req_i,
  input  logic [N-1:0]      tb_we_i,
  input  logic [N-1:0][31:0] tb_addr_i,
  input  logic [N-1:0][31:0] tb_wdata_i,
  output logic [N-1:0]      tb_rvalid_o,
  output logic [N-1:0]      tb_err_o,
  output logic [N-1:0][31:0] tb_rdata_o
);
  if (GX > 16 || GY > 16) begin : g_size_check
    $fatal(1, "soc_top: router grid %0dx%0d exceeds the 4-bit coordinates", GX, GY);
  end

  localparam int unsigned DEPTH = credit_rtt(LINK_SLICE) + 1;

  link_t x_out [G], y_out [G], x_in [G], y_in [G];
  logic  x_out_cr [G], y_out_cr [G], x_in_cr [G], y_in_cr [G];

  for (genvar y = 0; y < GY; y++) begin : g_y
    for (genvar x = 0; x < GX; x++) begin : g_x
      localparam int unsigned g  = y * GX + x;
      localparam int unsigned XP = y * GX + (x + GX - 1) % GX;   // node feeding our X input
      localparam int unsigned YP = ((y + GY - 1) % GY) * GX + x; // node feeding our Y input

      if (y < NY) begin : g_tile
        localparam int unsigned n = y * NX + x;
        tile_top #(.NX(NX), .NY(NY), .LINK_SLICE(LINK_SLICE)) u_tile (
          .clk_i, .rst_ni, .coord_x_i(4'(x)), .coord_y_i(4'(y)),
          .x_in_i(x_in[g]), .x_in_credit_o(x_in_cr[g]), .x_out_o(x_out[g]), .x_out_credit_i(x_out_cr[g]),
          .y_in_i(y_in[g]), .y_in_credit_o(y_in_cr[g]), .y_out_o(y_out[g]), .y_out_credit_i(y_out_cr[g]),
          .tb_req_i(tb_req_i[n]), .tb_we_i(tb_we_i[n]), .tb_addr_i(tb_addr_i[n]), .tb_wdata_i(tb_wdata_i[n]),
          .tb_rvalid_o(tb_rvalid_o[n]), .tb_err_o(tb_err_o[n]), .tb_rdata_o(tb_rdata_o[n])
        );
      end else begin : g_stop
        // Router-only stop in the I/O row
        link_t r_in [NPORTS], r_out [NPORTS];
        logic  r_cr_out [NPORTS], r_cr_in [NPORTS];
        logic [NPORTS-1:0] ev_flit, ev_cstall, ev_bhold, ev_bubble, ev_arbloss;
        assign r_in[IL]    = '0;
        assign r_in[IX]    = x_in[g];
        assign r_in[IY]    = y_in[g];
        assign x_in_cr[g]  = r_cr_out[IX];
        assign y_in_cr[g]  = r_cr_out[IY];
        assign x_out[g]    = r_out[IX];
        assign y_out[g]    = r_out[IY];
        assign r_cr_in[IL] = 1'b0;
        assign r_cr_in[IX] = x_out_cr[g];
        assign r_cr_in[IY] = y_out_cr[g];
        noc_router #(.DIR(1'b0), .GX(GX), .GY(GY), .DEPTH(DEPTH), .LINK_SLICE(LINK_SLICE),
                     .HAS_LOCAL(1'b0)) u_router (
          .clk_i, .rst_ni, .x_i(coord_t'(x)), .y_i(coord_t'(y)),
          .in_i(r_in), .credit_o(r_cr_out), .out_o(r_out), .credit_i(r_cr_in),
          .ev_flit_o(ev_flit), .ev_credit_stall_o(ev_cstall), .ev_bubble_hold_o(ev_bhold),
          .ev_bubble_o(ev_bubble), .ev_arb_loss_o(ev_arbloss)
        );
        /* verilator lint_off UNUSEDSIGNAL */
        logic unused;
        assign unused = ^{r_out[IL], r_cr_out[IL], ev_flit, ev_cstall, ev_bhold, ev_bubble, ev_arbloss};
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
