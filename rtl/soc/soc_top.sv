// Grid generator (SPEC.md, Baseline architecture): an NX x NY torus of compute
// tiles plus the I/O row, on two planes. The router grid is GX x GY with GX = NX
// and GY = NY + 1; row y = NY is the I/O row, part of every Y ring. On NoC0 each
// node's X ring output feeds the next node east and its Y ring output the next
// node south; on NoC1 west and north. Rings wrap around, with LINK_SLICE
// register slices on every link.
//
// The I/O row holds the I/O tile at (0, NY), whose AXI slave port is the
// design's boundary (in its own clock domain), and router-only stops (router,
// local port tied off) at the other positions.
//
// FT_ORDER routes links through the tiles' feedthroughs for a placement of the
// grid as hard tiles (M5): 0 none (links go directly, the tiles' feedthroughs
// are tied off), 1 logical order, 2 folded order (the rings of
// scripts/gen_fold_svg.py; a tile whose ring successor lies physically west or
// above is mirrored, as scripts/pnr.py places it). A link crosses every tile
// between its ends, plus an end tile whose port faces away from the other end
// (the U-turn at a fold or at a wraparound). Only wiring changes: the network
// behaves the same for every FT_ORDER (the SoC tests run with each).
module soc_top
  import noc_pkg::*;
#(
  parameter int unsigned NX         = 2,
  parameter int unsigned NY         = 2,
  parameter int unsigned LINK_SLICE = 0,
  parameter int unsigned FT_ORDER   = 0,
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

  if (FT_ORDER > 2) begin : g_ft_check
    $fatal(1, "soc_top: FT_ORDER %0d is not 0, 1 or 2", FT_ORDER);
  end

  // ---------------------------------------------- placement (FT_ORDER > 0)
  // Physical position along a dimension (0: X, columns left to right; 1: Y,
  // rows top to bottom), as scripts/pnr.py places tiles.
  function automatic int ring_slot(int i, int n, int r, int head);
    int k, m, j;
    k = (i - r + n) % n;
    m = n - head;
    if (k < head) return k;
    j = k - head;
    return head + ((2 * j <= m - 1) ? 2 * j : 2 * (m - 1 - j) + 1);
  endfunction
  function automatic int ring_len(int dim);
    return dim == 0 ? GX : GY;
  endfunction
  function automatic int ppos(int dim, int i);
    if (FT_ORDER != 2) return i;
    return dim == 0 ? ring_slot(i, GX, 0, 0) : ring_slot(i, GY, NY, 1);
  endfunction
  // Mirrored (MY in X, MX in Y): the ring successor lies physically before it
  function automatic bit mirrored(int dim, int i);
    return FT_ORDER == 2 && ppos(dim, (i + 1) % ring_len(dim)) < ppos(dim, i);
  endfunction
  // Side (+1: toward higher physical positions) of a tile's ring output and
  // input on plane p: NoC0 outputs east and south, NoC1 west and north
  function automatic int out_side(int dim, int p, int i);
    int side;
    side = p == 0 ? 1 : -1;
    return mirrored(dim, i) ? -side : side;
  endfunction
  function automatic int in_side(int dim, int p, int i);
    return -out_side(dim, p, i);
  endfunction
  // Does the link of plane p from s to its ring successor cross tile t? Only
  // compute tiles have feedthroughs (the I/O row in Y is standard cells).
  function automatic bit crosses(int dim, int p, int s, int t);
    int n, d, dir;
    if (FT_ORDER == 0 || (dim == 1 && t == NY)) return 0;
    n   = ring_len(dim);
    d   = p == 0 ? (s + 1) % n : (s + n - 1) % n;
    dir = ppos(dim, d) > ppos(dim, s) ? 1 : -1;
    if (t == s) return !(dim == 1 && s == NY) && out_side(dim, p, s) != dir;
    if (t == d) return !(dim == 1 && d == NY) && in_side(dim, p, d) == dir;
    return (ppos(dim, t) - ppos(dim, s)) * dir > 0 && (ppos(dim, d) - ppos(dim, t)) * dir > 0;
  endfunction
  function automatic int chain_len(int dim, int p, int s);
    int c;
    c = 0;
    for (int t = 0; t < ring_len(dim); t++) if (crosses(dim, p, s, t)) c++;
    return c;
  endfunction
  // k-th tile the link crosses, in its direction of travel
  function automatic int chain_tile(int dim, int p, int s, int k);
    int n, d, dir, c;
    n   = ring_len(dim);
    d   = p == 0 ? (s + 1) % n : (s + n - 1) % n;
    dir = ppos(dim, d) > ppos(dim, s) ? 1 : -1;
    c   = 0;
    for (int key = 0; key < n; key++)
      for (int t = 0; t < n; t++)
        if (crosses(dim, p, s, t) && (ppos(dim, t) - ppos(dim, s)) * dir == key) begin
          if (c == k) return t;
          c++;
        end
    return 0;
  endfunction
  // Feedthrough lane of tile t that runs physically toward +dir: lane 0 runs
  // west to east (north to south) as placed in R0
  // Is lane l of tile t (position along dim) crossed by some link?
  function automatic bit ft_used(int dim, int l, int t);
    for (int p = 0; p < 2; p++)
      for (int s = 0; s < ring_len(dim); s++)
        if (crosses(dim, p, s, t) && ft_lane(dim, p, s, t) == l) return 1;
    return 0;
  endfunction
  function automatic int ft_lane(int dim, int p, int s, int t);
    int n, d, dir, lane;
    n    = ring_len(dim);
    d    = p == 0 ? (s + 1) % n : (s + n - 1) % n;
    dir  = ppos(dim, d) > ppos(dim, s) ? 1 : -1;
    lane = dir > 0 ? 0 : 1;
    return mirrored(dim, t) ? 1 - lane : lane;
  endfunction
  localparam int unsigned IO_X  = 0;

  // Per plane p and node g
  link_t x_out [2][G], y_out [2][G], x_in [2][G], y_in [2][G];
  logic  x_out_cr [2][G], y_out_cr [2][G], x_in_cr [2][G], y_in_cr [2][G];
  // Feedthrough lanes per node (compute tiles only). split_var: chains run
  // from lane to lane (with FT_ORDER 1 a wraparound crosses every tile of its
  // ring), which is not a loop. The outputs of lanes no link crosses are unused.
  link_t xft_i [2][G] /* verilator split_var */, yft_i [2][G] /* verilator split_var */;
  logic  xft_cr_i [2][G] /* verilator split_var */, yft_cr_i [2][G] /* verilator split_var */;
  /* verilator lint_off UNUSEDSIGNAL */
  link_t xft_o [2][G] /* verilator split_var */, yft_o [2][G] /* verilator split_var */;
  logic  xft_cr_o [2][G] /* verilator split_var */, yft_cr_o [2][G] /* verilator split_var */;
  /* verilator lint_on UNUSEDSIGNAL */

  for (genvar y = 0; y < GY; y++) begin : g_y
    for (genvar x = 0; x < GX; x++) begin : g_x
      localparam int unsigned g = y * GX + x;

      if (y < NY) begin : g_tile
        link_t xi [2], xo [2], yi [2], yo [2];
        logic  xic [2], xoc [2], yic [2], yoc [2];
        for (genvar p = 0; p < 2; p++) begin : g_p
          assign xi[p] = x_in[p][g];
          assign yi[p] = y_in[p][g];
          assign xoc[p] = x_out_cr[p][g];
          assign yoc[p] = y_out_cr[p][g];
          assign x_out[p][g]  = xo[p];
          assign y_out[p][g]  = yo[p];
          assign x_in_cr[p][g] = xic[p];
          assign y_in_cr[p][g] = yic[p];
        end
        link_t fxi [2], fxo [2], fyi [2], fyo [2];
        logic  fxci [2], fxco [2], fyci [2], fyco [2];
        for (genvar l = 0; l < 2; l++) begin : g_ft
          assign fxi[l]  = xft_i[l][g];
          assign fyi[l]  = yft_i[l][g];
          assign fxci[l] = xft_cr_i[l][g];
          assign fyci[l] = yft_cr_i[l][g];
          assign xft_o[l][g]    = fxo[l];
          assign yft_o[l][g]    = fyo[l];
          assign xft_cr_o[l][g] = fxco[l];
          assign yft_cr_o[l][g] = fyco[l];
          // A lane no link crosses is tied off
          if (!ft_used(0, l, x)) begin : g_x_off
            assign xft_i[l][g]    = '0;
            assign xft_cr_i[l][g] = 1'b0;
          end
          if (!ft_used(1, l, y)) begin : g_y_off
            assign yft_i[l][g]    = '0;
            assign yft_cr_i[l][g] = 1'b0;
          end
        end
        tile_top #(.NX(NX), .NY(NY), .LINK_SLICE(LINK_SLICE), .IO_RD_OUTSTANDING(IO_RD_OUTSTANDING)) u_tile (
          .clk_i, .rst_ni, .coord_x_i(4'(x)), .coord_y_i(4'(y)),
          .x_in_i(xi), .x_in_credit_o(xic), .x_out_o(xo), .x_out_credit_i(xoc),
          .y_in_i(yi), .y_in_credit_o(yic), .y_out_o(yo), .y_out_credit_i(yoc),
          .xft_i(fxi), .xft_o(fxo), .xft_credit_i(fxci), .xft_credit_o(fxco),
          .yft_i(fyi), .yft_o(fyo), .yft_credit_i(fyci), .yft_credit_o(fyco)
        );
      end else begin : g_io
        // No feedthroughs in the I/O row
        for (genvar l = 0; l < 2; l++) begin : g_ft
          assign xft_i[l][g] = '0;  assign xft_cr_i[l][g] = 1'b0;
          assign yft_i[l][g] = '0;  assign yft_cr_i[l][g] = 1'b0;
          assign xft_o[l][g] = '0;  assign xft_cr_o[l][g] = 1'b0;
          assign yft_o[l][g] = '0;  assign yft_cr_o[l][g] = 1'b0;
        end
        // I/O row: the I/O tile's routers, or router-only stops
        localparam bit IO = x == IO_X;
        link_t inj [2], ej [2];
        logic  inj_cr [2], ej_cr [2];
        for (genvar p = 0; p < 2; p++) begin : g_p
          link_t r_in [NPORTS], r_out [NPORTS];
          logic  r_cr_out [NPORTS], r_cr_in [NPORTS];
          logic [NPORTS-1:0] ev_flit, ev_cstall, ev_bhold, ev_bubble, ev_arbloss;
          assign r_in[IL]       = inj[p];
          assign r_in[IX]       = x_in[p][g];
          assign r_in[IY]       = y_in[p][g];
          assign x_in_cr[p][g]  = r_cr_out[IX];
          assign y_in_cr[p][g]  = r_cr_out[IY];
          assign x_out[p][g]    = r_out[IX];
          assign y_out[p][g]    = r_out[IY];
          assign inj_cr[p]      = r_cr_out[IL];
          assign ej[p]          = r_out[IL];
          assign r_cr_in[IL]    = ej_cr[p];
          assign r_cr_in[IX]    = x_out_cr[p][g];
          assign r_cr_in[IY]    = y_out_cr[p][g];
          noc_router #(.DIR(p == 1), .GX(GX), .GY(GY), .DEPTH(DEPTH), .LINK_SLICE(LINK_SLICE),
                       .HAS_LOCAL(IO)) u_router (
            .clk_i, .rst_ni, .x_i(coord_t'(x)), .y_i(coord_t'(y)),
            .in_i(r_in), .credit_o(r_cr_out), .out_o(r_out), .credit_i(r_cr_in),
            .ev_flit_o(ev_flit), .ev_credit_stall_o(ev_cstall), .ev_bubble_hold_o(ev_bhold),
            .ev_bubble_o(ev_bubble), .ev_arb_loss_o(ev_arbloss)
          );
          /* verilator lint_off UNUSEDSIGNAL */
          logic unused;
          assign unused = ^{ev_flit, ev_cstall, ev_bhold, ev_bubble, ev_arbloss};
          /* verilator lint_on UNUSEDSIGNAL */
        end
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
            .clk_i, .rst_ni, .inj_o(inj), .inj_credit_i(inj_cr), .ej_i(ej), .ej_credit_o(ej_cr)
          );
        end else begin : g_stop
          for (genvar p = 0; p < 2; p++) begin : g_p
            assign inj[p]   = '0;
            assign ej_cr[p] = 1'b0;
          end
          /* verilator lint_off UNUSEDSIGNAL */
          logic unused_local;
          assign unused_local = ^{ej[0], ej[1], inj_cr[0], inj_cr[1]};
          /* verilator lint_on UNUSEDSIGNAL */
        end
      end

      // Links into this node, from the node before it on each ring
      // Links into this node, each behind its register slices; with FT_ORDER
      // the slice output runs through the feedthroughs of the tiles the link
      // crosses (in its direction of travel), and the credit back through the
      // same lanes.
      for (genvar p = 0; p < 2; p++) begin : g_link
        localparam int unsigned SX = p == 0 ? (x + GX - 1) % GX : (x + 1) % GX;
        localparam int unsigned SY = p == 0 ? (y + GY - 1) % GY : (y + 1) % GY;
        localparam int unsigned XP = y * GX + SX;
        localparam int unsigned YP = SY * GX + x;
        localparam int unsigned NXC = y < NY ? chain_len(0, p, SX) : 0;   // no feedthroughs in the I/O row
        localparam int unsigned NYC = chain_len(1, p, SY);
        link_t xs, ys;
        logic  xs_cr, ys_cr;
        link_slice #(.DEPTH(LINK_SLICE)) u_xlink (
          .clk_i, .rst_ni, .fwd_i(x_out[p][XP]), .fwd_o(xs), .credit_i(xs_cr), .credit_o(x_out_cr[p][XP])
        );
        link_slice #(.DEPTH(LINK_SLICE)) u_ylink (
          .clk_i, .rst_ni, .fwd_i(y_out[p][YP]), .fwd_o(ys), .credit_i(ys_cr), .credit_o(y_out_cr[p][YP])
        );

        if (NXC == 0) begin : g_x_direct
          assign x_in[p][g] = xs;
          assign xs_cr      = x_in_cr[p][g];
        end else begin : g_x_ft
          for (genvar k = 0; k < NXC; k++) begin : g_hop
            localparam int unsigned T  = chain_tile(0, p, SX, k);
            localparam int unsigned TG = y * GX + T;
            localparam int unsigned L  = ft_lane(0, p, SX, T);
            if (k == 0) begin : g_first
              assign xft_i[L][TG] = xs;
            end else begin : g_next
              localparam int unsigned TP = chain_tile(0, p, SX, k - 1);
              localparam int unsigned LP = ft_lane(0, p, SX, TP);
              localparam int unsigned GP = y * GX + TP;
              assign xft_i[L][TG] = xft_o[LP][GP];
            end
            if (k == NXC - 1) begin : g_last
              assign xft_cr_i[L][TG] = x_in_cr[p][g];
            end else begin : g_back
              localparam int unsigned TN = chain_tile(0, p, SX, k + 1);
              localparam int unsigned LN = ft_lane(0, p, SX, TN);
              localparam int unsigned GN = y * GX + TN;
              assign xft_cr_i[L][TG] = xft_cr_o[LN][GN];
            end
          end
          localparam int unsigned TF = chain_tile(0, p, SX, 0);
          localparam int unsigned TL = chain_tile(0, p, SX, NXC - 1);
          localparam int unsigned LF = ft_lane(0, p, SX, TF);
          localparam int unsigned LL = ft_lane(0, p, SX, TL);
          localparam int unsigned GF = y * GX + TF;
          localparam int unsigned GL = y * GX + TL;
          assign x_in[p][g] = xft_o[LL][GL];
          assign xs_cr      = xft_cr_o[LF][GF];
        end

        if (NYC == 0) begin : g_y_direct
          assign y_in[p][g] = ys;
          assign ys_cr      = y_in_cr[p][g];
        end else begin : g_y_ft
          for (genvar k = 0; k < NYC; k++) begin : g_hop
            localparam int unsigned T  = chain_tile(1, p, SY, k);
            localparam int unsigned TG = T * GX + x;
            localparam int unsigned L  = ft_lane(1, p, SY, T);
            if (k == 0) begin : g_first
              assign yft_i[L][TG] = ys;
            end else begin : g_next
              localparam int unsigned TP = chain_tile(1, p, SY, k - 1);
              localparam int unsigned LP = ft_lane(1, p, SY, TP);
              localparam int unsigned GP = TP * GX + x;
              assign yft_i[L][TG] = yft_o[LP][GP];
            end
            if (k == NYC - 1) begin : g_last
              assign yft_cr_i[L][TG] = y_in_cr[p][g];
            end else begin : g_back
              localparam int unsigned TN = chain_tile(1, p, SY, k + 1);
              localparam int unsigned LN = ft_lane(1, p, SY, TN);
              localparam int unsigned GN = TN * GX + x;
              assign yft_cr_i[L][TG] = yft_cr_o[LN][GN];
            end
          end
          localparam int unsigned TF = chain_tile(1, p, SY, 0);
          localparam int unsigned TL = chain_tile(1, p, SY, NYC - 1);
          localparam int unsigned LF = ft_lane(1, p, SY, TF);
          localparam int unsigned LL = ft_lane(1, p, SY, TL);
          localparam int unsigned GF = TF * GX + x;
          localparam int unsigned GL = TL * GX + x;
          assign y_in[p][g] = yft_o[LL][GL];
          assign ys_cr      = yft_cr_o[LF][GF];
        end
      end
    end
  end
endmodule
