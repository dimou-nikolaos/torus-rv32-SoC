// One credit-based link: sender (credit counter + output register), SLICE
// register slices, receiver input buffer of DEPTH entries. The testbench drives
// the source and the receiver's pops.
module tb_link_top
  import noc_pkg::*;
#(
  parameter int unsigned SLICE         = 0,
  parameter int unsigned DEPTH         = 4,
  parameter bit          ALLOW_SHALLOW = 1'b0   // negative tests only
) (
  input  logic         clk_i,
  input  logic         rst_ni,
  input  logic         src_valid_i,
  input  flit_t        src_flit_i,
  output logic         src_taken_o,
  output logic [3:0]   credits_o,
  input  logic         pop_i,
  output logic         rx_valid_o,     // link valid at the receiver this cycle
  output logic [3:0]   rx_count_o,
  output flit_t        rx_head_o,
  output logic         bubble_o
);
  // Elaboration-time check (SPEC.md, NoC full-throughput rule)
  if (!ALLOW_SHALLOW && DEPTH < credit_rtt(SLICE)) begin : g_shallow
    $fatal(1, "tb_link_top: DEPTH %0d is below the credit round trip %0d", DEPTH, credit_rtt(SLICE));
  end

  localparam int unsigned CW = $clog2(DEPTH + 1);
  logic [CW-1:0] credits;
  logic          send, credit_back, credit_q;
  link_t         out_q, rx;

  assign send        = src_valid_i && credits != '0;
  assign src_taken_o = send;
  // Bubble: a valid flit and a credit, yet nothing sent (zero by construction here)
  assign bubble_o    = src_valid_i && credits != '0 && !send;
  assign credits_o   = 4'(credits);

  credit_counter #(.DEPTH(DEPTH)) u_cc (
    .clk_i, .rst_ni, .send_i(send), .credit_i(credit_back), .credits_o(credits)
  );

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) out_q.valid <= 1'b0;
    else         out_q.valid <= send;
  always_ff @(posedge clk_i) begin
    out_q.la_port <= P_LOCAL;
    out_q.flit    <= src_flit_i;
  end

  link_slice #(.DEPTH(SLICE)) u_slice (
    .clk_i, .rst_ni, .fwd_i(out_q), .fwd_o(rx), .credit_i(credit_q), .credit_o(credit_back)
  );

  logic [$clog2(DEPTH > 1 ? DEPTH : 2):0] count;
  logic empty;
  noc_fifo #(.DEPTH(DEPTH), .T(flit_t)) u_fifo (
    .clk_i, .rst_ni, .push_i(rx.valid), .data_i(rx.flit), .pop_i(pop_i && !empty),
    .empty_o(empty), .head_o(rx_head_o), .count_o(count)
  );
  assign rx_count_o = 4'(count);
  assign rx_valid_o = rx.valid;

  // One credit per freed slot, registered
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) credit_q <= 1'b0;
    else         credit_q <= pop_i && !empty;

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^rx.la_port;
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
