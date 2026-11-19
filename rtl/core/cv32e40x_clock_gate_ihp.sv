// CV32E40X clock gate for place and route in IHP SG13G2: the library's
// integrated clock gate (latch and AND), replacing the behavioral bhv/ model.
module cv32e40x_clock_gate #(
  parameter int LIB = 0
) (
  input  logic clk_i,
  input  logic en_i,
  input  logic scan_cg_en_i,
  output logic clk_o
);
  sg13g2_lgcp_1 u_icg (.CLK(clk_i), .GATE(en_i | scan_cg_en_i), .GCLK(clk_o));
endmodule
