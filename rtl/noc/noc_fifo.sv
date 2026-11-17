// Input buffer of a credit-based link: DEPTH entries, written from the link,
// read at the head. Overflow is a protocol violation (the sender ran out of
// credits it did not have) and is asserted in simulation.
module noc_fifo #(
  parameter int unsigned DEPTH = 4,
  parameter type         T     = logic,
  localparam int unsigned AW   = (DEPTH > 1) ? $clog2(DEPTH) : 1
) (
  input  logic clk_i,
  input  logic rst_ni,
  input  logic push_i,
  input  T     data_i,
  input  logic pop_i,
  output logic empty_o,
  output T     head_o,
  output logic [AW:0] count_o
);
  T            mem [DEPTH];
  logic [AW-1:0] rd_q, wr_q;
  logic [AW:0]   cnt_q;

  function automatic logic [AW-1:0] incr(logic [AW-1:0] p);
    return (int'(p) == int'(DEPTH) - 1) ? '0 : p + 1'b1;
  endfunction

  assign empty_o = cnt_q == '0;
  assign head_o  = mem[rd_q];
  assign count_o = cnt_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rd_q  <= '0;
      wr_q  <= '0;
      cnt_q <= '0;
    end else begin
      if (push_i) wr_q <= incr(wr_q);
      if (pop_i)  rd_q <= incr(rd_q);
      cnt_q <= cnt_q + (AW + 1)'(push_i) - (AW + 1)'(pop_i);
    end
  end

  always_ff @(posedge clk_i)
    if (push_i) mem[wr_q] <= data_i;

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni) begin
      assert (!(push_i && !pop_i && int'(cnt_q) == int'(DEPTH)))
        else $error("noc_fifo: overflow (sender used a credit it did not have)");
      assert (!(pop_i && cnt_q == '0)) else $error("noc_fifo: pop while empty");
    end
`endif
endmodule
