// Bare NoC: one plane (direction DIR) or both planes (PLANES = 2: NoC0 and
// NoC1 side by side), with a testbench endpoint at every node and plane: a
// sender with a credit counter and output register into the local port, and an
// ejection buffer the testbench pops. With IO_ROW, the routers in row GY-1
// other than (0, GY-1) are router-only stops and their endpoints stay idle.
// All per-node signals are flattened for C++, index k = plane * N + node.
module tb_noc_top
  import noc_pkg::*;
#(
  parameter bit          DIR    = 1'b0,
  parameter int unsigned PLANES = 1,
  parameter int unsigned GX     = 2,
  parameter int unsigned GY     = 2,
  parameter int unsigned SLICE  = 0,
  parameter bit          BUBBLE = 1'b1,
  parameter bit          IO_ROW = 1'b0,
  localparam int unsigned DEPTH = credit_rtt(SLICE) + 1,
  localparam int unsigned N     = GX * GY,
  localparam int unsigned K     = PLANES * N
) (
  input  logic                 clk_i,
  input  logic                 rst_ni,
  input  logic [K-1:0]         src_valid_i,
  input  flit_t [K-1:0]        src_flit_i,
  output logic [K-1:0]         src_taken_o,
  input  logic [K-1:0]         pop_i,
  output logic [K-1:0]         rx_valid_o,    // ejection buffer not empty
  output flit_t [K-1:0]        rx_head_o,
  output logic [K-1:0][NPORTS-1:0] ev_flit_o,
  output logic [K-1:0][NPORTS-1:0] ev_bubble_hold_o,
  output logic [K-1:0][NPORTS-1:0] ev_bubble_o,
  output logic [K-1:0][NPORTS-1:0] ev_credit_stall_o
);
  if (PLANES != 1 && PLANES != 2) begin : g_planes_check
    $fatal(1, "tb_noc_top: PLANES must be 1 or 2");
  end

  localparam int unsigned CW = $clog2(DEPTH + 1);
  localparam int unsigned AW = (DEPTH > 1) ? $clog2(DEPTH) : 1;

  for (genvar p = 0; p < PLANES; p++) begin : g_plane
    link_t inj [N], ej [N];
    logic  inj_credit [N], ej_credit [N];
    logic [NPORTS-1:0] evf [N], evh [N], evb [N], evc [N];

    for (genvar n = 0; n < N; n++) begin : g_node
      localparam int unsigned k = p * N + n;
      localparam coord_t X = coord_t'(n % GX);
      localparam coord_t Y = coord_t'(n / GX);
      // Sender: the injection's lookahead port is the route at this router
      logic [CW-1:0] credits;
      logic          send;
      link_t         q;
      assign send           = src_valid_i[k] && credits != '0;
      assign src_taken_o[k] = send;
      credit_counter #(.DEPTH(DEPTH)) u_cc (
        .clk_i, .rst_ni, .send_i(send), .credit_i(inj_credit[n]), .credits_o(credits)
      );
      always_ff @(posedge clk_i or negedge rst_ni)
        if (!rst_ni) q.valid <= 1'b0;
        else         q.valid <= send;
      always_ff @(posedge clk_i) begin
        q.flit    <= src_flit_i[k];
        q.la_port <= route(X, Y, src_flit_i[k].dst_x, src_flit_i[k].dst_y);
      end
      assign inj[n] = q;

      // Ejection buffer
      logic empty, cr_q;
      logic [AW:0] count;
      flit_t head;
      noc_fifo #(.DEPTH(DEPTH), .T(flit_t)) u_ej (
        .clk_i, .rst_ni, .push_i(ej[n].valid), .data_i(ej[n].flit), .pop_i(pop_i[k] && !empty),
        .empty_o(empty), .head_o(head), .count_o(count)
      );
      always_ff @(posedge clk_i or negedge rst_ni)
        if (!rst_ni) cr_q <= 1'b0;
        else         cr_q <= pop_i[k] && !empty;
      assign ej_credit[n]  = cr_q;
      assign rx_valid_o[k] = !empty;
      assign rx_head_o[k]  = head;
      assign ev_flit_o[k]  = evf[n];
      assign ev_bubble_hold_o[k] = evh[n];
      assign ev_bubble_o[k] = evb[n];
      assign ev_credit_stall_o[k] = evc[n];
      /* verilator lint_off UNUSEDSIGNAL */
      logic [AW:0] unused_count;
      logic [1:0]  unused_la;
      assign unused_count = count;
      assign unused_la    = ej[n].la_port;
      /* verilator lint_on UNUSEDSIGNAL */
    end

    noc_plane #(.DIR(PLANES == 2 ? p == 1 : DIR), .GX(GX), .GY(GY), .LINK_SLICE(SLICE), .DEPTH(DEPTH),
                .BUBBLE(BUBBLE), .IO_ROW(IO_ROW)) u_plane (
      .clk_i, .rst_ni, .inj_i(inj), .inj_credit_o(inj_credit), .ej_o(ej), .ej_credit_i(ej_credit),
      .ev_flit_o(evf), .ev_bubble_hold_o(evh), .ev_bubble_o(evb), .ev_credit_stall_o(evc)
    );
  end
endmodule
