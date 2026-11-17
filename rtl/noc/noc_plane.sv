// One NoC plane: a GX x GY torus of noc_router, one-way rings in the plane's
// direction, LINK_SLICE register slices on every router-to-router link. The
// local ports are the plane's boundary (NIUs, or testbench endpoints).
module noc_plane
  import noc_pkg::*;
#(
  parameter bit          DIR        = 1'b0,
  parameter int unsigned GX         = 2,
  parameter int unsigned GY         = 2,
  parameter int unsigned LINK_SLICE = 0,
  parameter int unsigned DEPTH      = credit_rtt(LINK_SLICE) + 1,
  parameter bit          BUBBLE     = 1'b1,
  localparam int unsigned N         = GX * GY   // node n = y * GX + x
) (
  input  logic  clk_i,
  input  logic  rst_ni,
  // Local ports, per node: injection into the router, ejection out of it
  input  link_t inj_i        [N],
  output logic  inj_credit_o [N],
  output link_t ej_o         [N],
  input  logic  ej_credit_i  [N],
  // Router events, per node and port
  output logic [NPORTS-1:0] ev_flit_o        [N],
  output logic [NPORTS-1:0] ev_bubble_hold_o [N],
  output logic [NPORTS-1:0] ev_bubble_o      [N],
  output logic [NPORTS-1:0] ev_credit_stall_o [N]
);
  link_t in_l   [N][NPORTS];
  link_t out_l  [N][NPORTS];
  logic  cr_out [N][NPORTS];   // router -> its upstream
  logic  cr_in  [N][NPORTS];   // downstream -> router

  function automatic int unsigned node(int unsigned x, int unsigned y);
    return y * GX + x;
  endfunction

  for (genvar y = 0; y < GY; y++) begin : g_y
    for (genvar x = 0; x < GX; x++) begin : g_x
      localparam int unsigned n  = node(x, y);
      // The router feeding this one on each ring
      localparam int unsigned XP = DIR ? (x + 1) % GX : (x + GX - 1) % GX;
      localparam int unsigned YP = DIR ? (y + 1) % GY : (y + GY - 1) % GY;
      logic [NPORTS-1:0] ev_al;

      noc_router #(.DIR(DIR), .GX(GX), .GY(GY), .DEPTH(DEPTH), .LINK_SLICE(LINK_SLICE),
                   .BUBBLE(BUBBLE)) u_router (
        .clk_i, .rst_ni, .x_i(coord_t'(x)), .y_i(coord_t'(y)),
        .in_i(in_l[n]), .credit_o(cr_out[n]), .out_o(out_l[n]), .credit_i(cr_in[n]),
        .ev_flit_o(ev_flit_o[n]), .ev_credit_stall_o(ev_credit_stall_o[n]),
        .ev_bubble_hold_o(ev_bubble_hold_o[n]), .ev_bubble_o(ev_bubble_o[n]),
        .ev_arb_loss_o(ev_al)
      );
      /* verilator lint_off UNUSEDSIGNAL */
      logic unused;
      assign unused = ^ev_al;
      /* verilator lint_on UNUSEDSIGNAL */

      // Local port
      assign in_l[n][IL]  = inj_i[n];
      assign inj_credit_o[n] = cr_out[n][IL];
      assign ej_o[n]      = out_l[n][IL];
      assign cr_in[n][IL] = ej_credit_i[n];

      // X ring: this router's X input comes from the previous router's X output
      link_slice #(.DEPTH(LINK_SLICE)) u_xlink (
        .clk_i, .rst_ni,
        .fwd_i(out_l[node(XP, y)][IX]), .fwd_o(in_l[n][IX]),
        .credit_i(cr_out[n][IX]), .credit_o(cr_in[node(XP, y)][IX])
      );
      link_slice #(.DEPTH(LINK_SLICE)) u_ylink (
        .clk_i, .rst_ni,
        .fwd_i(out_l[node(x, YP)][IY]), .fwd_o(in_l[n][IY]),
        .credit_i(cr_out[n][IY]), .credit_o(cr_in[node(x, YP)][IY])
      );
    end
  end
endmodule
