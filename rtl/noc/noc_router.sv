// One plane's router: 3x3 (local, X ring, Y ring), two stages per hop
// (SPEC.md, Router).
//
//   cycle 1  the flit sits at the head of its input buffer; the switch
//            allocator grants it if its output has the credits it needs, and
//            the lookahead port for the next router is computed
//   cycle 2  the flit is in the output register, which drives the link
//
// Each input buffer head names its output (the lookahead port that came with
// it), so each input requests one output and allocation is a round-robin per
// output. Allowed paths, X first: X -> X/Y/local, Y -> Y/local, local -> any
// (local -> local is the NIU loopback).
//
// Bubble rule: a flit entering a ring -- local injection into X or Y, or the
// X-to-Y turn -- needs two credits on that output; a flit continuing on its
// ring, or ejecting, needs one. This keeps every ring from filling up, which
// makes the one-way torus deadlock-free without virtual channels.
module noc_router
  import noc_pkg::*;
#(
  parameter bit          DIR       = 1'b0,  // 0: NoC0 (+x, +y), 1: NoC1 (-x, -y)
  parameter int unsigned GX        = 2,     // ring lengths
  parameter int unsigned GY        = 2,
  parameter int unsigned DEPTH     = 5,     // every input buffer (and downstream buffer)
  parameter int unsigned LINK_SLICE = 0,    // register slices on the links, for the depth check
  parameter bit          HAS_LOCAL = 1'b1,  // 0: router-only stop, local port tied off
  parameter bit          BUBBLE    = 1'b1   // 0 only in the test that shows the deadlock it prevents
) (
  input  logic          clk_i,
  input  logic          rst_ni,
  input  coord_t        x_i,
  input  coord_t        y_i,
  // Inputs, indexed by port_e: the flit arrives with its output port here
  input  link_t         in_i     [NPORTS],
  output logic          credit_o [NPORTS],   // to the upstream sender
  // Outputs: the flit leaves with its output port at the next router
  output link_t         out_o    [NPORTS],
  input  logic          credit_i [NPORTS],   // from the downstream buffer
  // Events (one bit per cycle) for the counters and the testbench
  output logic [NPORTS-1:0] ev_flit_o,        // per output: flit sent
  output logic [NPORTS-1:0] ev_credit_stall_o, // per output: requested, no credit at all
  output logic [NPORTS-1:0] ev_bubble_hold_o, // per output: an entering flit held by the bubble rule
  output logic [NPORTS-1:0] ev_bubble_o,      // per output: eligible flit, nothing sent (bug)
  output logic [NPORTS-1:0] ev_arb_loss_o     // per input: eligible, lost arbitration
);
  // A flit entering a ring needs two credits, so one slot is never usable by an
  // injection stream: full rate for it needs the round trip plus that slot.
  if (DEPTH < credit_rtt(LINK_SLICE) + 1) begin : g_depth_check
    $fatal(1, "noc_router: DEPTH %0d is below the credit round trip %0d plus the bubble slot",
           DEPTH, credit_rtt(LINK_SLICE));
  end

  localparam int unsigned CW = $clog2(DEPTH + 1);
  localparam int unsigned AW = (DEPTH > 1) ? $clog2(DEPTH) : 1;

  typedef struct packed {
    port_e la_port;
    flit_t flit;
  } entry_t;

  // ------------------------------------------------------------ input buffers
  logic   empty [NPORTS];
  entry_t head  [NPORTS];
  logic   pop   [NPORTS];

  for (genvar i = 0; i < NPORTS; i++) begin : g_in
    logic [AW:0] count;
    noc_fifo #(.DEPTH(DEPTH), .T(entry_t)) u_fifo (
      .clk_i, .rst_ni,
      .push_i(in_i[i].valid && (HAS_LOCAL || i != IL)),
      .data_i('{la_port: in_i[i].la_port, flit: in_i[i].flit}),
      .pop_i(pop[i]), .empty_o(empty[i]), .head_o(head[i]), .count_o(count)
    );
    // One credit per freed slot, registered
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) credit_o[i] <= 1'b0;
      else         credit_o[i] <= pop[i];
    /* verilator lint_off UNUSEDSIGNAL */
    logic [AW:0] unused_count;
    assign unused_count = count;
    /* verilator lint_on UNUSEDSIGNAL */
  end

  // ---------------------------------------------------------------- credits
  logic [CW-1:0] credits [NPORTS];
  logic          send    [NPORTS];
  for (genvar o = 0; o < NPORTS; o++) begin : g_cc
    credit_counter #(.DEPTH(DEPTH)) u_cc (
      .clk_i, .rst_ni, .send_i(send[o]), .credit_i(credit_i[o]), .credits_o(credits[o])
    );
  end

  // ------------------------------------------------------- switch allocation
  // A flit entering the ring of output o: injection into X or Y, or the turn X -> Y.
  function automatic logic entering(int unsigned i, int unsigned o);
    return (o == IX && i == IL) || (o == IY && (i == IL || i == IX));
  endfunction

  function automatic logic legal(int unsigned i, int unsigned o);
    return !(i == IY && o == IX);
  endfunction

  logic [NPORTS-1:0] cand  [NPORTS];   // [o][i]: head of input i wants output o
  logic [NPORTS-1:0] elig  [NPORTS];   // [o][i]: ... and has the credits it needs
  logic [NPORTS-1:0] grant [NPORTS];   // [o][i]
  logic [$clog2(NPORTS)-1:0] rr_q [NPORTS];  // last input granted, per output

  always_comb begin
    for (int o = 0; o < NPORTS; o++) begin
      for (int i = 0; i < NPORTS; i++) begin
        cand[o][i] = !empty[i] && int'(head[i].la_port) == o;
        elig[o][i] = cand[o][i] &&
                     ((BUBBLE && entering(i, o)) ? credits[o] >= CW'(2) : credits[o] != '0);
      end
      // Round-robin: first eligible input after the last one granted
      grant[o] = '0;
      for (int k = 1; k <= NPORTS; k++)
        if (grant[o] == '0 && elig[o][(int'(rr_q[o]) + k) % NPORTS])
          grant[o][(int'(rr_q[o]) + k) % NPORTS] = 1'b1;
      send[o] = |grant[o];
    end
    for (int i = 0; i < NPORTS; i++) begin
      pop[i] = 1'b0;
      for (int o = 0; o < NPORTS; o++) pop[i] |= grant[o][i];
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      for (int o = 0; o < NPORTS; o++) rr_q[o] <= '0;
    end else begin
      for (int o = 0; o < NPORTS; o++)
        for (int i = 0; i < NPORTS; i++)
          if (grant[o][i]) rr_q[o] <= ($clog2(NPORTS))'(i);
    end

  // ----------------------------------------------- traversal and lookahead
  // The winner of each output goes to its output register together with the
  // port it will take at the next router on that ring.
  function automatic port_e next_port(int unsigned o, coord_t dst_x, coord_t dst_y);
    coord_t nx, ny;
    nx = x_i;
    ny = y_i;
    if (o == IX) nx = ring_next(x_i, GX, DIR);
    if (o == IY) ny = ring_next(y_i, GY, DIR);
    return route(nx, ny, dst_x, dst_y);
  endfunction

  link_t out_q [NPORTS];
  for (genvar o = 0; o < NPORTS; o++) begin : g_out
    /* verilator lint_off UNUSEDSIGNAL */
    entry_t win;   // its la_port is this router's, not needed past allocation
    /* verilator lint_on UNUSEDSIGNAL */
    always_comb begin
      win = head[0];
      for (int i = 0; i < NPORTS; i++) if (grant[o][i]) win = head[i];
    end
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) out_q[o].valid <= 1'b0;
      else         out_q[o].valid <= send[o] && (HAS_LOCAL || o != IL);
    always_ff @(posedge clk_i)
      if (send[o]) begin
        out_q[o].flit    <= win.flit;
        out_q[o].la_port <= next_port(o, win.flit.dst_x, win.flit.dst_y);
      end
    assign out_o[o] = out_q[o];
  end

  // ----------------------------------------------------------------- events
  always_comb
    for (int o = 0; o < NPORTS; o++) begin
      ev_flit_o[o]         = send[o];
      ev_credit_stall_o[o] = |cand[o] && credits[o] == '0;
      ev_bubble_hold_o[o]  = 1'b0;
      for (int i = 0; i < NPORTS; i++)
        ev_bubble_hold_o[o] |= BUBBLE && cand[o][i] && entering(i, o) && credits[o] == CW'(1);
      ev_bubble_o[o]       = |elig[o] && !send[o];
    end
  always_comb
    for (int i = 0; i < NPORTS; i++) begin
      ev_arb_loss_o[i] = 1'b0;
      for (int o = 0; o < NPORTS; o++) ev_arb_loss_o[i] |= elig[o][i] && !grant[o][i];
    end

`ifndef SYNTHESIS
  // Protocol checks
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni) begin
      for (int i = 0; i < NPORTS; i++)
        if (!empty[i]) begin
          assert (legal(i, int'(head[i].la_port)))
            else $error("noc_router (%0d,%0d): illegal path input %0d -> output %0d", x_i, y_i, i, head[i].la_port);
          assert (head[i].la_port == route(x_i, y_i, head[i].flit.dst_x, head[i].flit.dst_y))
            else $error("noc_router (%0d,%0d): input %0d lookahead port %0d, route says %0d",
                        x_i, y_i, i, head[i].la_port, route(x_i, y_i, head[i].flit.dst_x, head[i].flit.dst_y));
        end
      assert (ev_bubble_o == '0) else $error("noc_router (%0d,%0d): bubble on outputs %b", x_i, y_i, ev_bubble_o);
      if (!HAS_LOCAL) assert (!in_i[IL].valid) else $error("noc_router: flit on a tied-off local port");
    end
`endif

endmodule
