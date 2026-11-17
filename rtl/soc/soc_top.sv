// NX x NY torus of compute tiles on NoC0 (M1). Each tile's X ring output feeds
// the next tile east, its Y ring output the next tile south, wrapping around,
// with LINK_SLICE register slices on every link. The I/O row arrives in M2.
//
// The per-tile testbench register ports (tb_*) are M1 scaffolding.
module soc_top
  import noc_pkg::*;
#(
  parameter int unsigned NX         = 2,
  parameter int unsigned NY         = 2,
  parameter int unsigned LINK_SLICE = 0,
  localparam int unsigned N         = NX * NY   // tile n = y * NX + x
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
  link_t x_out [N], y_out [N], x_in [N], y_in [N];
  logic  x_out_cr [N], y_out_cr [N], x_in_cr [N], y_in_cr [N];

  for (genvar y = 0; y < NY; y++) begin : g_y
    for (genvar x = 0; x < NX; x++) begin : g_x
      localparam int unsigned n  = y * NX + x;
      localparam int unsigned XP = y * NX + (x + NX - 1) % NX;   // tile feeding our X input
      localparam int unsigned YP = ((y + NY - 1) % NY) * NX + x; // tile feeding our Y input

      tile_top #(.GX(NX), .GY(NY), .LINK_SLICE(LINK_SLICE)) u_tile (
        .clk_i, .rst_ni, .coord_x_i(4'(x)), .coord_y_i(4'(y)),
        .x_in_i(x_in[n]), .x_in_credit_o(x_in_cr[n]), .x_out_o(x_out[n]), .x_out_credit_i(x_out_cr[n]),
        .y_in_i(y_in[n]), .y_in_credit_o(y_in_cr[n]), .y_out_o(y_out[n]), .y_out_credit_i(y_out_cr[n]),
        .tb_req_i(tb_req_i[n]), .tb_we_i(tb_we_i[n]), .tb_addr_i(tb_addr_i[n]), .tb_wdata_i(tb_wdata_i[n]),
        .tb_rvalid_o(tb_rvalid_o[n]), .tb_err_o(tb_err_o[n]), .tb_rdata_o(tb_rdata_o[n])
      );

      link_slice #(.DEPTH(LINK_SLICE)) u_xlink (
        .clk_i, .rst_ni, .fwd_i(x_out[XP]), .fwd_o(x_in[n]), .credit_i(x_in_cr[n]), .credit_o(x_out_cr[XP])
      );
      link_slice #(.DEPTH(LINK_SLICE)) u_ylink (
        .clk_i, .rst_ni, .fwd_i(y_out[YP]), .fwd_o(y_in[n]), .credit_i(y_in_cr[n]), .credit_o(y_out_cr[YP])
      );
    end
  end
endmodule
