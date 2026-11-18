// CV32E40X clock gate for place and route in sky130: the library's integrated
// clock gate (latch and AND), replacing the behavioral bhv/ model.
module cv32e40x_clock_gate #(
  parameter int LIB = 0
) (
  input  logic clk_i,
  input  logic en_i,
  input  logic scan_cg_en_i,
  output logic clk_o
);
  sky130_fd_sc_hd__dlclkp_1 u_icg (.CLK(clk_i), .GATE(en_i | scan_cg_en_i), .GCLK(clk_o));
endmodule
