// I/O tile types and constants (SPEC.md, I/O tile).
//
// Global address on the AXI port (32 bits):
//   [31:28] tile x   [27:24] tile y   [23:20] window   [19:16] zero   [15:0] offset
//   window 0: scratchpad, local address = offset
//   window 1: registers,  local address = 0x1000_0000 | offset
//             (compute tiles: tile control and the counter bank; the I/O tile:
//             its register block at 0x1000_4000)
// Anything else, router-only stops, and the NIU registers answer SLVERR.
package io_pkg;

  localparam int unsigned AXI_DW  = 128;   // one beat = one flit payload
  localparam int unsigned AXI_IDW = 8;     // widest AXI ID carried through the crossing
  localparam int unsigned RD_CHUNK_BEATS = 4;  // a READ_REQ asks for at most 64 bytes

  // I/O register block (local addresses at the I/O tile, 4 KB page)
  localparam logic [31:0] IO_REG_BASE  = 32'h1000_4000;
  localparam logic [3:0]  IO_REG_PAGE  = 4'h4;
  localparam int unsigned IO_NSEM      = 16;            // 0x000 + 4k: host semaphores, SEM[0] = DONE
  localparam logic [11:0] IO_INFO      = 12'h100;       // RO: version, GX, GY, IO_RD_OUTSTANDING
  localparam logic [11:0] IO_CYCLES    = 12'h104;       // RO: fabric cycles (low 32 bits)
  localparam logic [11:0] IO_WR_FLITS  = 12'h108;       // RO: WRITE flits injected
  localparam logic [11:0] IO_RD_REQS   = 12'h10C;       // RO: READ_REQ flits injected
  localparam logic [11:0] IO_RSP_FLITS = 12'h110;       // RO: READ_RSP flits received
  localparam logic [11:0] IO_RX_ERRORS = 12'h114;       // RO: flits dropped
  localparam logic [11:0] IO_ATOMICS   = 12'h118;       // RO: ATOMIC_INC flits executed
  localparam logic [7:0]  IO_VERSION   = 8'h01;

  // Front-end to bridge, through the request crossing: one write beat, or one
  // whole read burst
  typedef struct packed {
    logic               rd;
    logic               err;     // read burst to answer with SLVERR
    logic [AXI_IDW-1:0] id;      // read
    logic [7:0]         len;     // read: beats - 1
    logic [31:0]        addr;    // global address of the (first) beat, 16-byte aligned
    logic [15:0]        strb;    // write
    logic [AXI_DW-1:0]  data;    // write
  } io_req_t;

  // Bridge to front-end, through the response crossing: one read beat
  typedef struct packed {
    logic [AXI_IDW-1:0] id;
    logic               last;
    logic               err;
    logic [AXI_DW-1:0]  data;
  } io_rsp_t;

  // Address checks shared by the front-end (SLVERR) and the testbench
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic io_addr_ok(logic [31:0] a, int unsigned gx, int unsigned gy,
                                      int unsigned io_x, int unsigned io_y);
    logic [3:0] x, y, win, hi, page;
    logic       io;
    x    = a[31:28];
    y    = a[27:24];
    win  = a[23:20];
    hi   = a[19:16];
    page = a[15:12];
    if (int'(x) >= int'(gx) || int'(y) >= int'(gy) || hi != '0) return 1'b0;
    io = int'(x) == int'(io_x) && int'(y) == int'(io_y);
    if (int'(y) == int'(io_y) && !io) return 1'b0;            // router-only stop
    if (win == 4'd0) return !io;                               // scratchpad
    if (win == 4'd1) return io ? page == IO_REG_PAGE : (page == 4'd1 || page == 4'd2);
    return 1'b0;
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

endpackage
