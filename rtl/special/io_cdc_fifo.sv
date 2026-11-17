// Asynchronous FIFO: the design's only clock-domain crossing (SPEC.md, I/O
// tile). DEPTH entries (a power of two) of type T, written in the wclk domain
// and read in the rclk domain. Each side keeps a binary pointer and its Gray
// copy; the Gray pointer crosses through SYNC flip-flops, so at most one bit
// changes per step and the far side sees either the old or the new value.
// The full and empty flags are conservative: a slot freed or filled on the far
// side shows up a few cycles later, never early.
//
// Each side has its own asynchronous reset; both must be applied together.
module io_cdc_fifo #(
  parameter type         T     = logic [31:0],
  parameter int unsigned DEPTH = 8,
  parameter int unsigned SYNC  = 2,
  localparam int unsigned AW   = $clog2(DEPTH)
) (
  // Write side
  input  logic wclk_i,
  input  logic wrst_ni,
  input  logic push_i,
  input  T     data_i,
  output logic full_o,
  // Read side
  input  logic rclk_i,
  input  logic rrst_ni,
  input  logic pop_i,
  output T     head_o,
  output logic empty_o
);
  if (DEPTH < 2 || (1 << AW) != DEPTH || SYNC < 2) begin : g_check
    $fatal(1, "io_cdc_fifo: DEPTH must be a power of two >= 2 and SYNC >= 2");
  end

  function automatic logic [AW:0] bin2gray(logic [AW:0] b);
    return b ^ (b >> 1);
  endfunction

  T mem [DEPTH];

  // ------------------------------------------------------------ write side
  logic [AW:0] wbin_q, wgray_q;
  logic [AW:0] rgray_w [SYNC];   // read pointer synchronized into wclk

  always_ff @(posedge wclk_i or negedge wrst_ni)
    if (!wrst_ni) begin
      wbin_q  <= '0;
      wgray_q <= '0;
    end else if (push_i) begin
      wbin_q  <= wbin_q + 1'b1;
      wgray_q <= bin2gray(wbin_q + 1'b1);
    end

  always_ff @(posedge wclk_i)
    if (push_i) mem[wbin_q[AW-1:0]] <= data_i;

  always_ff @(posedge wclk_i or negedge wrst_ni)
    if (!wrst_ni) rgray_w <= '{default: '0};
    else begin
      rgray_w[0] <= rgray_q;
      for (int i = 1; i < SYNC; i++) rgray_w[i] <= rgray_w[i-1];
    end

  // Full: the write pointer is one lap ahead of the read pointer (Gray: the
  // two top bits differ, the rest are equal)
  localparam logic [AW:0] LAP = (AW + 1)'(3) << (AW - 1);
  assign full_o = (wgray_q ^ rgray_w[SYNC-1]) == LAP;

  // ------------------------------------------------------------- read side
  logic [AW:0] rbin_q, rgray_q;
  logic [AW:0] wgray_r [SYNC];   // write pointer synchronized into rclk

  always_ff @(posedge rclk_i or negedge rrst_ni)
    if (!rrst_ni) begin
      rbin_q  <= '0;
      rgray_q <= '0;
    end else if (pop_i) begin
      rbin_q  <= rbin_q + 1'b1;
      rgray_q <= bin2gray(rbin_q + 1'b1);
    end

  always_ff @(posedge rclk_i or negedge rrst_ni)
    if (!rrst_ni) wgray_r <= '{default: '0};
    else begin
      wgray_r[0] <= wgray_q;
      for (int i = 1; i < SYNC; i++) wgray_r[i] <= wgray_r[i-1];
    end

  assign empty_o = rgray_q == wgray_r[SYNC-1];
  assign head_o  = mem[rbin_q[AW-1:0]];

`ifndef SYNTHESIS
  always_ff @(posedge wclk_i or negedge wrst_ni)
    if (wrst_ni) assert (!(push_i && full_o)) else $error("io_cdc_fifo: push while full");
  always_ff @(posedge rclk_i or negedge rrst_ni)
    if (rrst_ni) assert (!(pop_i && empty_o)) else $error("io_cdc_fifo: pop while empty");
`endif
endmodule
