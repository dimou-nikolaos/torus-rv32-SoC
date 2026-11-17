// One router with a full link on each port: upstream senders (credit counter +
// output register) and SLICE slices into each input, SLICE slices and a
// downstream input buffer after each output. The testbench drives the sources
// and the downstream pops; every port is flattened for the C++ side.
module tb_router_top
  import noc_pkg::*;
#(
  parameter bit          DIR   = 1'b0,
  parameter int unsigned GX    = 4,
  parameter int unsigned GY    = 4,
  parameter int unsigned SLICE = 0,
  parameter int unsigned DEPTH = 5
) (
  input  logic                clk_i,
  input  logic                rst_ni,
  input  logic [3:0]          x_i,
  input  logic [3:0]          y_i,
  // sources, per input port
  input  logic [NPORTS-1:0]   src_valid_i,
  input  logic [NPORTS-1:0][1:0] src_la_i,
  input  flit_t [NPORTS-1:0]  src_flit_i,
  output logic [NPORTS-1:0]   src_taken_o,
  // downstream buffers, per output port
  input  logic [NPORTS-1:0]   pop_i,
  output logic [NPORTS-1:0][3:0] rx_count_o,
  output logic [NPORTS-1:0][1:0] rx_head_la_o,
  output flit_t [NPORTS-1:0]  rx_head_o,
  // router internals for the cycle comparison
  output logic [NPORTS-1:0][3:0] up_credits_o,   // upstream senders' credits
  output logic [NPORTS-1:0]   out_valid_o,       // router output registers
  output logic [NPORTS-1:0]   ev_flit_o,
  output logic [NPORTS-1:0]   ev_credit_stall_o,
  output logic [NPORTS-1:0]   ev_bubble_hold_o,
  output logic [NPORTS-1:0]   ev_bubble_o,
  output logic [NPORTS-1:0]   ev_arb_loss_o
);
  localparam int unsigned CW = $clog2(DEPTH + 1);
  localparam int unsigned AW = (DEPTH > 1) ? $clog2(DEPTH) : 1;

  link_t r_in [NPORTS], r_out [NPORTS];
  logic  r_credit_o [NPORTS], r_credit_i [NPORTS];

  for (genvar p = 0; p < NPORTS; p++) begin : g_port
    // ---------------------------------------------------------- upstream sender
    logic [CW-1:0] credits;
    logic          send, credit_back;
    link_t         up_q;
    assign send           = src_valid_i[p] && credits != '0;
    assign src_taken_o[p] = send;
    assign up_credits_o[p] = 4'(credits);
    credit_counter #(.DEPTH(DEPTH)) u_cc (
      .clk_i, .rst_ni, .send_i(send), .credit_i(credit_back), .credits_o(credits)
    );
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) up_q.valid <= 1'b0;
      else         up_q.valid <= send;
    always_ff @(posedge clk_i) begin
      up_q.la_port <= port_e'(src_la_i[p]);
      up_q.flit    <= src_flit_i[p];
    end
    link_slice #(.DEPTH(SLICE)) u_in_slice (
      .clk_i, .rst_ni, .fwd_i(up_q), .fwd_o(r_in[p]), .credit_i(r_credit_o[p]), .credit_o(credit_back)
    );

    // ------------------------------------------------------- downstream buffer
    link_t   dn;
    logic    dn_credit_q, empty;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [AW:0] count;   // DEPTH <= 9 here, so 4 bits of it are enough
    /* verilator lint_on UNUSEDSIGNAL */
    typedef struct packed { port_e la; flit_t flit; } ent_t;
    ent_t    head;
    link_slice #(.DEPTH(SLICE)) u_out_slice (
      .clk_i, .rst_ni, .fwd_i(r_out[p]), .fwd_o(dn), .credit_i(dn_credit_q), .credit_o(r_credit_i[p])
    );
    noc_fifo #(.DEPTH(DEPTH), .T(ent_t)) u_dn (
      .clk_i, .rst_ni, .push_i(dn.valid), .data_i('{la: dn.la_port, flit: dn.flit}),
      .pop_i(pop_i[p] && !empty), .empty_o(empty), .head_o(head), .count_o(count)
    );
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) dn_credit_q <= 1'b0;
      else         dn_credit_q <= pop_i[p] && !empty;
    assign rx_count_o[p]   = 4'(count);
    assign rx_head_la_o[p] = head.la;
    assign rx_head_o[p]    = head.flit;
    assign out_valid_o[p]  = r_out[p].valid;
  end

  noc_router #(.DIR(DIR), .GX(GX), .GY(GY), .DEPTH(DEPTH), .LINK_SLICE(SLICE)) u_dut (
    .clk_i, .rst_ni, .x_i(coord_t'(x_i)), .y_i(coord_t'(y_i)),
    .in_i(r_in), .credit_o(r_credit_o), .out_o(r_out), .credit_i(r_credit_i),
    .ev_flit_o, .ev_credit_stall_o, .ev_bubble_hold_o, .ev_bubble_o, .ev_arb_loss_o
  );
endmodule
