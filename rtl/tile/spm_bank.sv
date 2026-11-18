// One scratchpad bank: ROWS x 128 bit, single port, byte enables, read data in
// the cycle after the access only: SRAM macros do not hold it, so simulation
// builds invert it in every other cycle and a user that reads it late fails.
// Simulation builds export a DPI backdoor for the testbench ELF loader and
// memory dumps. With SPM_SKY130 defined the bank is built from sky130 SRAM
// macros (spm_bank_sky130), without the backdoor.
module spm_bank #(
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
`ifdef SPM_SKY130
  spm_bank_sky130 #(.ROWS(ROWS)) u_macros (.*);
`else
  logic [127:0] mem [ROWS];

  always_ff @(posedge clk_i) begin
    if (en_i) begin
      if (we_i) begin
        for (int b = 0; b < 16; b++)
          if (be_i[b]) mem[row_i][b*8 +: 8] <= wdata_i[b*8 +: 8];
      end else begin
        rdata_o <= mem[row_i];
      end
    end
`ifndef SYNTHESIS
    if (!(en_i && !we_i)) rdata_o <= ~rdata_o;
`endif
  end

`ifndef SYNTHESIS
  // Backdoor, 32-bit granularity: word index within this bank's row space.
  export "DPI-C" function spm_bank_bd_write;
  export "DPI-C" function spm_bank_bd_read;

  function void spm_bank_bd_write(input int row, input int lane, input int data);
    mem[row[AW-1:0]][lane[1:0]*32 +: 32] = data;
  endfunction

  function int spm_bank_bd_read(input int row, input int lane);
    return mem[row[AW-1:0]][lane[1:0]*32 +: 32];
  endfunction
`endif
`endif

endmodule
