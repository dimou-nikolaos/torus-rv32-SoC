// Sender-side credit counter for one link: starts at the receiver's buffer
// depth, -1 per flit sent, +1 per credit returned. Going negative or above the
// buffer depth means a lost or duplicated credit, asserted in simulation.
module credit_counter #(
  parameter int unsigned DEPTH = 4,
  localparam int unsigned CW   = $clog2(DEPTH + 1)
) (
  input  logic          clk_i,
  input  logic          rst_ni,
  input  logic          send_i,
  input  logic          credit_i,
  output logic [CW-1:0] credits_o
);
  logic [CW-1:0] cnt_q;
  assign credits_o = cnt_q;

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) cnt_q <= CW'(DEPTH);
    else         cnt_q <= cnt_q - CW'(send_i) + CW'(credit_i);

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni) begin
      assert (!(send_i && !credit_i && cnt_q == '0)) else $error("credit_counter: send without credit");
      assert (!(credit_i && !send_i && int'(cnt_q) == int'(DEPTH))) else $error("credit_counter: credit overflow");
    end
`endif
endmodule
