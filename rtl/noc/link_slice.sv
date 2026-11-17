// Register slices on a link: DEPTH stages on the forward half (flit, valid,
// lookahead port) and DEPTH stages on the credit wire. DEPTH = 0 is a wire.
// Each stage adds one cycle each way, so the credit round trip grows by 2.
module link_slice
  import noc_pkg::*;
#(
  parameter int unsigned DEPTH = 0
) (
  input  logic  clk_i,
  input  logic  rst_ni,
  input  link_t fwd_i,
  output link_t fwd_o,
  input  logic  credit_i,
  output logic  credit_o
);
  if (DEPTH == 0) begin : g_wire
    assign fwd_o    = fwd_i;
    assign credit_o = credit_i;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused;
    assign unused = clk_i ^ rst_ni;
    /* verilator lint_on UNUSEDSIGNAL */
  end else begin : g_regs
    link_t fwd_q [DEPTH];
    logic  cr_q  [DEPTH];
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) begin
        for (int i = 0; i < DEPTH; i++) begin
          fwd_q[i].valid <= 1'b0;
          cr_q[i]        <= 1'b0;
        end
      end else begin
        fwd_q[0].valid <= fwd_i.valid;
        cr_q[0]        <= credit_i;
        for (int i = 1; i < DEPTH; i++) begin
          fwd_q[i].valid <= fwd_q[i-1].valid;
          cr_q[i]        <= cr_q[i-1];
        end
      end
    // Payload without reset: only meaningful with valid
    always_ff @(posedge clk_i) begin
      fwd_q[0].la_port <= fwd_i.la_port;
      fwd_q[0].flit    <= fwd_i.flit;
      for (int i = 1; i < DEPTH; i++) begin
        fwd_q[i].la_port <= fwd_q[i-1].la_port;
        fwd_q[i].flit    <= fwd_q[i-1].flit;
      end
    end
    assign fwd_o    = fwd_q[DEPTH-1];
    assign credit_o = cr_q[DEPTH-1];
  end
endmodule
