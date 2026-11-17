// Async FIFO alone, 32-bit entries; the testbench drives both clocks.
module tb_cdc_top #(
  parameter int unsigned DEPTH = 8
) (
  input  logic        wclk_i,
  input  logic        wrst_ni,
  input  logic        push_i,
  input  logic [31:0] data_i,
  output logic        full_o,
  input  logic        rclk_i,
  input  logic        rrst_ni,
  input  logic        pop_i,
  output logic [31:0] head_o,
  output logic        empty_o
);
  io_cdc_fifo #(.T(logic [31:0]), .DEPTH(DEPTH)) u_dut (.*);
endmodule
