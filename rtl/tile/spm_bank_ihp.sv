// One scratchpad bank built from IHP SG13G2 SRAM macros (place and route, M5):
// two RM_IHPSG13_1P_<ROWS>x64 side by side, 64 bits each (lanes 0-1 and 2-3),
// their bit masks from the byte enables. 512 rows (32 KB scratchpads) or 1024
// (64 KB). Same ports and timing as the behavioral spm_bank, which instantiates
// this under SPM_IHP. The BIST port is tied off; A_DLY must be 1.
//
// The macro registers its read data at the rising edge and holds it, more than
// spm_bank promises (the cycle after the read).
module spm_bank_ihp #(
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
  logic [127:0] bm;
  for (genvar b = 0; b < 16; b++) begin : g_bm
    assign bm[8*b +: 8] = {8{be_i[b]}};
  end

  for (genvar h = 0; h < 2; h++) begin : g_half
    if (ROWS == 512) begin : g_512
      RM_IHPSG13_1P_512x64_c2_bm_bist u_ram (
        .A_CLK(clk_i), .A_MEN(en_i), .A_WEN(we_i), .A_REN(!we_i), .A_ADDR(row_i),
        .A_DIN(wdata_i[64*h +: 64]), .A_DLY(1'b1), .A_DOUT(rdata_o[64*h +: 64]), .A_BM(bm[64*h +: 64]),
        .A_BIST_CLK(1'b0), .A_BIST_EN(1'b0), .A_BIST_MEN(1'b0), .A_BIST_WEN(1'b0), .A_BIST_REN(1'b0),
        .A_BIST_ADDR('0), .A_BIST_DIN('0), .A_BIST_BM('0)
      );
    end else if (ROWS == 1024) begin : g_1024
      RM_IHPSG13_1P_1024x64_c2_bm_bist u_ram (
        .A_CLK(clk_i), .A_MEN(en_i), .A_WEN(we_i), .A_REN(!we_i), .A_ADDR(row_i),
        .A_DIN(wdata_i[64*h +: 64]), .A_DLY(1'b1), .A_DOUT(rdata_o[64*h +: 64]), .A_BM(bm[64*h +: 64]),
        .A_BIST_CLK(1'b0), .A_BIST_EN(1'b0), .A_BIST_MEN(1'b0), .A_BIST_WEN(1'b0), .A_BIST_REN(1'b0),
        .A_BIST_ADDR('0), .A_BIST_DIN('0), .A_BIST_BM('0)
      );
    end else begin : g_bad_rows
      $fatal(1, "spm_bank_ihp: ROWS (%0d) must be 512 or 1024", ROWS);
    end
  end

endmodule
