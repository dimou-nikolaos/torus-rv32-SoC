// Tile-level constants and types: local memory map, scratchpad geometry and the
// 128-bit scratchpad master port shared by every bank-arbiter client.
package tile_pkg;

  // ---------------------------------------------------------------- memory map
  localparam logic [31:0] SPM_BASE      = 32'h0000_0000;
  localparam int unsigned SPM_BYTES     = 64 * 1024;
  localparam logic [31:0] NIU_BASE      = 32'h1000_0000;  // 256 B, local core only
  localparam logic [31:0] TCTRL_BASE    = 32'h1000_1000;  // 256 B
  localparam logic [31:0] PERF_BASE     = 32'h1000_2000;  // 4 KB, read-only
  localparam logic [31:0] CONSOLE_BASE  = 32'h1000_3000;  // 256 B, simulation only

  // MMIO decode: everything in 0x1000_xxxx, region selected by bits [15:12]
  localparam logic [15:0] MMIO_HI       = 16'h1000;
  typedef enum logic [3:0] {
    MMIO_NIU     = 4'h0,
    MMIO_TCTRL   = 4'h1,
    MMIO_PERF    = 4'h2,
    MMIO_CONSOLE = 4'h3
  } mmio_region_e;

  // Tile control register offsets
  localparam logic [7:0] TCTRL_COORD    = 8'h00;  // RO: x in [7:0], y in [15:8]
  localparam logic [7:0] TCTRL_CTRL     = 8'h04;  // bit 0 = run
  localparam logic [7:0] TCTRL_CYCLE_LO = 8'h08;  // RO
  localparam logic [7:0] TCTRL_CYCLE_HI = 8'h0C;  // RO, latched when CYCLE_LO is read
  localparam logic [7:0] TCTRL_DOORBELL = 8'h10;  // write 1s to clear
  localparam logic [7:0] TCTRL_GRID     = 8'h14;  // RO: compute NX [7:0], NY [15:8], I/O tile x [23:16], y [31:24]

  // Console registers (simulation only)
  localparam logic [7:0] CONSOLE_TX     = 8'h00;  // write: one character
  localparam logic [7:0] CONSOLE_EXIT   = 8'h04;  // write: end simulation with this code

  // ---------------------------------------------------------------- scratchpad
  localparam int unsigned SPM_BANKS      = 4;
  localparam int unsigned SPM_WORD_BYTES = 16;                          // 128-bit bank word
  localparam int unsigned SPM_ROWS       = SPM_BYTES / SPM_WORD_BYTES / SPM_BANKS;  // 1024
  localparam int unsigned SPM_AW         = $clog2(SPM_BYTES);           // 16
  localparam int unsigned SPM_ROW_AW     = $clog2(SPM_ROWS);            // 10
  // Byte address split: [3:0] byte in word, [5:4] bank, [15:6] row
  localparam int unsigned SPM_BANK_LSB   = $clog2(SPM_WORD_BYTES);      // 4
  localparam int unsigned SPM_ROW_LSB    = SPM_BANK_LSB + $clog2(SPM_BANKS);  // 6

  // Bank-arbiter master ports (SPEC.md). Default priority: engine, TX-DMA and
  // TX-reply (round-robin), RX0 and RX1 (round-robin), core data, core instruction.
  localparam int unsigned SPM_PORTS   = 7;
  localparam int unsigned PORT_ENGINE = 0;
  localparam int unsigned PORT_RX0    = 1;
  localparam int unsigned PORT_RX1    = 2;
  localparam int unsigned PORT_TXDMA  = 3;
  localparam int unsigned PORT_TXREP  = 4;
  localparam int unsigned PORT_CORE_D = 5;
  localparam int unsigned PORT_CORE_I = 6;

  // Scratchpad master port. OBI-like: a request is held until granted; read
  // data returns exactly one cycle after the grant. `lock` on a granted request
  // reserves the bank for the same port in the next cycle (atomic RMW); that
  // next request may be a write with `add`, which stores read data + wdata in
  // the one 32-bit lane its byte enables select.
  typedef struct packed {
    logic                  req;
    logic                  we;
    logic                  lock;
    logic                  add;
    logic [SPM_AW-1:0]     addr;   // byte address; [3:0] ignored
    logic [15:0]           be;
    logic [127:0]          wdata;
  } spm_req_t;

  typedef struct packed {
    logic                  gnt;
    logic                  rvalid;
    logic [127:0]          rdata;
  } spm_rsp_t;

  // Register port for MMIO blocks: always ready, response one cycle later.
  typedef struct packed {
    logic        req;
    logic        we;
    logic [11:0] addr;   // offset within the 4 KB region
    logic [3:0]  be;
    logic [31:0] wdata;
  } reg_req_t;

  typedef struct packed {
    logic        rvalid;
    logic        err;
    logic [31:0] rdata;
  } reg_rsp_t;

  // ------------------------------------------------------- performance counters
  // Tile counter bank (rtl/perf/counter_bank.sv): counters 0 and 1 count cycles
  // and retired instructions, counters 2..9 count the event their select names.
  // Event numbers (the select values):
  typedef enum int unsigned {
    EV_CYCLES      = 0,   // cycles with CTRL.run set (fixed counter 0)
    EV_RETIRED     = 1,   // retired instructions (fixed counter 1)
    EV_FETCH_STALL = 2,   // IF had no valid instruction for ID
    EV_DATA_STALL  = 3,   // WB stalled on a data access
    EV_DENIED_0    = 4,   // 4..10: arbiter denials per port
    EV_PROMOTED_0  = 4 + SPM_PORTS,  // 11..17: grants through promotion per port
    EV_NIU_TX0     = 4 + 2 * SPM_PORTS,      // 18, 19: flits injected by the NIU on NoC0, NoC1
    EV_NIU_TX1     = 5 + 2 * SPM_PORTS,
    EV_NIU_RX0     = 6 + 2 * SPM_PORTS,      // 20, 21: flits retired by the NIU from NoC0, NoC1
    EV_NIU_RX1     = 7 + 2 * SPM_PORTS,
    EV_NIU_ERR     = 8 + 2 * SPM_PORTS,      // 22: cycles with a flit dropped (unsupported op or address)
    EV_R0_FLIT     = 9 + 2 * SPM_PORTS,      // 23..25: NoC0 router flits per output (L, X, Y)
    EV_R0_CSTALL   = 12 + 2 * SPM_PORTS,     // 26..28: per output, waiting with no credit
    EV_R0_ARBLOSS  = 15 + 2 * SPM_PORTS,     // 29..31: per input, eligible but lost arbitration
    EV_R0_BHOLD    = 18 + 2 * SPM_PORTS,     // 32..34: per output, entering flit held by the bubble rule
    EV_R1_FLIT     = 21 + 2 * SPM_PORTS,     // 35..46: the same for the NoC1 router
    EV_R1_CSTALL   = 24 + 2 * SPM_PORTS,
    EV_R1_ARBLOSS  = 27 + 2 * SPM_PORTS,
    EV_R1_BHOLD    = 30 + 2 * SPM_PORTS,
    EV_NUM         = 33 + 2 * SPM_PORTS
  } perf_event_e;
  localparam int unsigned PERF_NFIX  = 2;
  localparam int unsigned PERF_NPROG = 8;
  // Selects after reset: the counters most runs want
  localparam logic [PERF_NPROG-1:0][7:0] PERF_DEFAULT_SEL = '{
    8'(EV_DENIED_0 + PORT_CORE_D), 8'(EV_NIU_ERR), 8'(EV_NIU_RX1), 8'(EV_NIU_RX0),
    8'(EV_NIU_TX1), 8'(EV_NIU_TX0), 8'(EV_DATA_STALL), 8'(EV_FETCH_STALL)
  };
  localparam logic [11:0] PERF_SEL_BASE     = 12'h400;
  localparam logic [11:0] PERF_SNAP_BASE    = 12'h800;
  localparam logic [11:0] PERF_NUM_OFFSET   = 12'hFF0;
  localparam logic [11:0] PERF_CMD_OFFSET   = 12'hFF4;
  localparam logic [11:0] PERF_NEV_OFFSET   = 12'hFF8;
  localparam int unsigned PERF_CMD_SNAPSHOT = 0;
  localparam int unsigned PERF_CMD_CLEAR    = 1;

  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [$clog2(SPM_BANKS)-1:0] spm_bank_of(logic [SPM_AW-1:0] a);
    return a[SPM_ROW_LSB-1:SPM_BANK_LSB];
  endfunction

  function automatic logic [SPM_ROW_AW-1:0] spm_row_of(logic [SPM_AW-1:0] a);
    return a[SPM_AW-1:SPM_ROW_LSB];
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

endpackage
