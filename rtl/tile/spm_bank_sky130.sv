// One scratchpad bank built from sky130 OpenRAM macros (place and route, M5):
// sky130_sram_2kbyte_1rw1r_32x512_8, four wide (one per 32-bit lane, its byte
// mask from the lane's byte enables) and ROWS / 512 deep (row MSBs select the
// macro row). Only the read-write port is used. Same ports and timing as the
// behavioral spm_bank, which instantiates this under SPM_SKY130.
//
// The macro latches its inputs at the rising edge and drives the read data
// after the falling edge, valid until the next rising edge: the cycle after the
// read, which is all spm_bank promises.
module spm_bank_sky130 #(
  parameter int unsigned ROWS = tile_pkg::SPM_ROWS,
  localparam int unsigned AW  = $clog2(ROWS)
) (
  input  logic          clk_i,
  input  logic          en_i,
  input  logic          we_i,
  input  logic [AW-1:0] row_i,
  input  logic [15:0]   be_i,
  input  logic [127:0]  wdata_i,
  output logic [127:0]  rdata_o
);
  localparam int unsigned MROWS = 512;
  localparam int unsigned DEPTH = ROWS / MROWS;
  localparam int unsigned SW    = DEPTH > 1 ? $clog2(DEPTH) : 1;

  if (ROWS % MROWS != 0) begin : g_bad_rows
    $fatal(1, "spm_bank_sky130: ROWS (%0d) must be a multiple of %0d", ROWS, MROWS);
  end

  logic [SW-1:0] sel, sel_q;
  logic [127:0]  dout [DEPTH];

  assign sel = DEPTH > 1 ? SW'(row_i >> 9) : '0;

  for (genvar d = 0; d < DEPTH; d++) begin : g_row
    logic csb;
    assign csb = !(en_i && sel == SW'(d));
    for (genvar l = 0; l < 4; l++) begin : g_lane
      // Port 1 (read only) is unused: deselected, its clock tied low.
      /* verilator lint_off PINCONNECTEMPTY */
`ifdef SYNTHESIS
      sky130_sram_2kbyte_1rw1r_32x512_8 u_ram (
`else
      sky130_sram_2kbyte_1rw1r_32x512_8 #(.VERBOSE(0)) u_ram (
`endif
        .clk0(clk_i), .csb0(csb), .web0(!we_i), .wmask0(be_i[4*l +: 4]),
        .addr0(row_i[8:0]), .din0(wdata_i[32*l +: 32]), .dout0(dout[d][32*l +: 32]),
        .clk1(1'b0), .csb1(1'b1), .addr1(9'h0), .dout1()
      );
      /* verilator lint_on PINCONNECTEMPTY */
    end
  end

  always_ff @(posedge clk_i) sel_q <= sel;

  assign rdata_o = dout[sel_q];

endmodule
