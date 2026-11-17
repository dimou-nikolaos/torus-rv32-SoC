// Small SystemVerilog check for the yosys-slang frontend: packages, structs,
// enums, interfaces-free generate, always_ff/always_comb.
package smoke_pkg;
  typedef enum logic [1:0] {IDLE, RUN, DONE} state_e;
  typedef struct packed { logic [3:0] x; logic [3:0] y; } xy_t;
endpackage

module slang_smoke import smoke_pkg::*; #(parameter int N = 4) (
  input  logic        clk, rst_n,
  input  xy_t         in_xy,
  output logic [N-1:0] onehot,
  output state_e      state
);
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) state <= IDLE;
    else        state <= (state == IDLE) ? RUN : DONE;
  always_comb begin
    onehot = '0;
    onehot[in_xy.x[$clog2(N)-1:0]] = 1'b1;
  end
endmodule
