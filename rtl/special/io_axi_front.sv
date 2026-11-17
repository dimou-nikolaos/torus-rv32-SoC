// AXI4 slave front-end of the I/O tile (SPEC.md, I/O tile), in the AXI clock
// domain. Turns AXI transactions into the request stream of the crossing and
// returns read beats from the response stream.
//
// Supported: INCR bursts of full-width beats (AxSIZE = 4, 16 bytes), any
// length; byte strobes on writes. A burst whose start address fails the global
// address check (io_pkg::io_addr_ok), or with another size or burst type, is
// answered with SLVERR: a write consumes its beats and sends nothing; a read
// still travels through the bridge so responses stay in order, and returns
// zeros. A burst never crosses a 4 KB boundary (AXI rule), so its start
// address decides for every beat.
//
// Writes: one burst at a time; each beat is pushed as soon as it arrives, and
// B is sent once the last beat is in the crossing (posted: everything that
// follows is behind it on its way to the tile). Reads: one request per burst;
// read beats return in request order. When a write beat and a read request
// both wait, they alternate.
module io_axi_front
  import io_pkg::*;
#(
  parameter int unsigned GX   = 2,
  parameter int unsigned GY   = 3,
  parameter int unsigned IO_X = 0,
  parameter int unsigned IO_Y = 2,
  parameter int unsigned IDW  = 4
) (
  input  logic              clk_i,     // AXI clock
  input  logic              rst_ni,
  // AXI4 slave
  input  logic [IDW-1:0]    awid_i,
  input  logic [31:0]       awaddr_i,
  input  logic [7:0]        awlen_i,
  input  logic [2:0]        awsize_i,
  input  logic [1:0]        awburst_i,
  input  logic              awvalid_i,
  output logic              awready_o,
  input  logic [127:0]      wdata_i,
  input  logic [15:0]       wstrb_i,
  input  logic              wlast_i,
  input  logic              wvalid_i,
  output logic              wready_o,
  output logic [IDW-1:0]    bid_o,
  output logic [1:0]        bresp_o,
  output logic              bvalid_o,
  input  logic              bready_i,
  input  logic [IDW-1:0]    arid_i,
  input  logic [31:0]       araddr_i,
  input  logic [7:0]        arlen_i,
  input  logic [2:0]        arsize_i,
  input  logic [1:0]        arburst_i,
  input  logic              arvalid_i,
  output logic              arready_o,
  output logic [IDW-1:0]    rid_o,
  output logic [127:0]      rdata_o,
  output logic [1:0]        rresp_o,
  output logic              rlast_o,
  output logic              rvalid_o,
  input  logic              rready_i,
  // Request crossing (write side)
  output logic              req_push_o,
  output io_req_t           req_o,
  input  logic              req_full_i,
  // Response crossing (read side)
  input  io_rsp_t           rsp_i,
  input  logic              rsp_empty_i,
  output logic              rsp_pop_o
);
  if (IDW > AXI_IDW) begin : g_idw_check
    $fatal(1, "io_axi_front: IDW %0d exceeds the %0d ID bits the crossing carries", IDW, AXI_IDW);
  end

  localparam logic [1:0] RESP_OKAY = 2'b00, RESP_SLVERR = 2'b10;
  localparam logic [1:0] BURST_INCR = 2'b01;

  function automatic logic burst_ok(logic [31:0] addr, logic [2:0] size, logic [1:0] burst);
    return size == 3'd4 && burst == BURST_INCR && io_addr_ok(addr, GX, GY, IO_X, IO_Y);
  endfunction

  // ------------------------------------------------------------------ write
  logic           w_busy_q, w_bad_q, b_valid_q, b_bad_q;
  logic [IDW-1:0] w_id_q;
  logic [31:0]    w_addr_q;
  logic [7:0]     w_left_q;      // beats after this one

  // ------------------------------------------------------------------- read
  // Arbitration for the request crossing: a good write beat or a read request
  logic w_want, ar_want, pick_ar, last_ar_q;
  assign w_want  = w_busy_q && !w_bad_q && wvalid_i;
  assign ar_want = arvalid_i;
  assign pick_ar = ar_want && (!w_want || !last_ar_q);

  assign awready_o = !w_busy_q && !b_valid_q;
  assign wready_o  = w_busy_q && (w_bad_q || (!req_full_i && !pick_ar));
  assign arready_o = !req_full_i && pick_ar;

  logic aw_hs, w_hs, ar_hs;
  assign aw_hs = awvalid_i && awready_o;
  assign w_hs  = wvalid_i && wready_o;
  assign ar_hs = arvalid_i && arready_o;

  always_comb begin
    req_push_o = 1'b0;
    req_o      = '0;
    if (ar_hs) begin
      req_push_o = 1'b1;
      req_o.rd   = 1'b1;
      req_o.err  = !burst_ok(araddr_i, arsize_i, arburst_i);
      req_o.id   = AXI_IDW'(arid_i);
      req_o.len  = arlen_i;
      req_o.addr = {araddr_i[31:4], 4'h0};
    end else if (w_hs && !w_bad_q) begin
      req_push_o = 1'b1;
      req_o.addr = w_addr_q;
      req_o.strb = wstrb_i;
      req_o.data = wdata_i;
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      w_busy_q  <= 1'b0;
      w_bad_q   <= 1'b0;
      b_valid_q <= 1'b0;
      b_bad_q   <= 1'b0;
      w_id_q    <= '0;
      w_addr_q  <= '0;
      w_left_q  <= '0;
      last_ar_q <= 1'b0;
    end else begin
      if (aw_hs) begin
        w_busy_q <= 1'b1;
        w_bad_q  <= !burst_ok(awaddr_i, awsize_i, awburst_i);
        w_id_q   <= awid_i;
        w_addr_q <= {awaddr_i[31:4], 4'h0};
        w_left_q <= awlen_i;
      end
      if (w_hs) begin
        w_addr_q <= w_addr_q + 32'd16;
        w_left_q <= w_left_q - 1'b1;
        if (w_left_q == '0) begin
          w_busy_q  <= 1'b0;
          b_valid_q <= 1'b1;
          b_bad_q   <= w_bad_q;
        end
      end
      if (b_valid_q && bready_i) b_valid_q <= 1'b0;
      if (w_want && ar_want) last_ar_q <= pick_ar;
    end

  assign bvalid_o = b_valid_q;
  assign bid_o    = w_id_q;
  assign bresp_o  = b_bad_q ? RESP_SLVERR : RESP_OKAY;

  // --------------------------------------------------------------- read data
  assign rvalid_o  = !rsp_empty_i;
  assign rid_o     = IDW'(rsp_i.id);
  assign rdata_o   = rsp_i.data;
  assign rresp_o   = rsp_i.err ? RESP_SLVERR : RESP_OKAY;
  assign rlast_o   = rsp_i.last;
  assign rsp_pop_o = rvalid_o && rready_i;

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni && w_hs)
      assert (wlast_i == (w_left_q == '0))
        else $error("io_axi_front: WLAST %0d with %0d beats left", wlast_i, w_left_q);
`endif

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{awaddr_i[3:0], araddr_i[3:0], rsp_i.id};
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
