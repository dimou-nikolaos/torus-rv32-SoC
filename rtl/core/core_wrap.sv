// Thin wrapper around CV32E40X, configured RV32IM (Zca is always present in the
// core; the SDK compiles for rv32im so it is never used).
//
// The port list is identical in every flow:
//   - simulation instantiates the vendor wrapper (bhv/cv32e40x_wrapper.sv), which
//     adds the RVFI tracer at u_cv32.rvfi_i; the testbench taps it hierarchically
//   - synthesis (SYNTHESIS defined) instantiates cv32e40x_core directly
//
// CV-X-IF is instantiated and tied off here; M5 brings it out to the engine.
module core_wrap #(
  parameter logic [31:0] BOOT_ADDR  = 32'h0000_0000,
  parameter logic [31:0] MTVEC_ADDR = 32'h0000_0000,
  parameter logic [31:0] SPM_BYTES  = 32'h0001_0000
) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic        fetch_enable_i,
  input  logic [31:0] hartid_i,

  // Instruction OBI (read-only)
  output logic        instr_req_o,
  input  logic        instr_gnt_i,
  input  logic        instr_rvalid_i,
  output logic [31:0] instr_addr_o,
  input  logic [31:0] instr_rdata_i,
  input  logic        instr_err_i,

  // Data OBI
  output logic        data_req_o,
  input  logic        data_gnt_i,
  input  logic        data_rvalid_i,
  output logic [31:0] data_addr_o,
  output logic [3:0]  data_be_o,
  output logic        data_we_o,
  output logic [31:0] data_wdata_o,
  input  logic [31:0] data_rdata_i,
  input  logic        data_err_i,

  output logic        core_sleep_o,

  // Core performance events, tapped from the controller (one bit per cycle each)
  output logic        ev_retire_o,
  output logic        ev_fetch_stall_o,
  output logic        ev_data_stall_o
);
  import cv32e40x_pkg::*;

  // Scratchpad is main memory; everything else (MMIO at 0x1000_0000 and up) falls
  // back to the I/O default: no speculative or misaligned access, no fetch.
  localparam int unsigned PMA_NUM_REGIONS = 1;
  localparam pma_cfg_t PMA_CFG[PMA_NUM_REGIONS-1:0] = '{
    '{word_addr_low : 32'h0,
      word_addr_high: SPM_BYTES >> 2,
      main          : 1'b1,
      bufferable    : 1'b0,
      cacheable     : 1'b0,
      atomic        : 1'b0}
  };

  // Unused OBI side-band outputs
  /* verilator lint_off UNUSEDSIGNAL */
  logic [1:0] instr_memtype, data_memtype;
  logic [2:0] instr_prot, data_prot;
  logic       instr_dbg, data_dbg;
  logic [5:0] data_atop;
  logic [63:0] mcycle;
  logic       fencei_flush_req;
  logic       dbg_havereset, dbg_running, dbg_halted, dbg_pc_valid;
  logic [31:0] dbg_pc;
  /* verilator lint_on UNUSEDSIGNAL */

  // CV-X-IF, tied off until M5 (X_EXT = 0, so the core ignores it)
  cv32e40x_if_xif xif ();
  assign xif.compressed_ready = 1'b0;
  assign xif.compressed_resp  = '0;
  assign xif.issue_ready      = 1'b0;
  assign xif.issue_resp       = '0;
  assign xif.mem_valid        = 1'b0;
  assign xif.mem_req          = '0;
  assign xif.result_valid     = 1'b0;
  assign xif.result           = '0;

`ifdef SYNTHESIS
  cv32e40x_core #(
`else
  cv32e40x_wrapper #(
    .CORE_LOG_ENABLE (1'b0),
`endif
    .RV32            (RV32I),
    .A_EXT           (A_NONE),
    .B_EXT           (B_NONE),
    .M_EXT           (M),
    .X_EXT           (1'b0),
    .DEBUG           (1'b0),
    .CLIC            (1'b0),
    .PMA_NUM_REGIONS (PMA_NUM_REGIONS),
    .PMA_CFG         (PMA_CFG)
  ) u_cv32 (
    .clk_i               (clk_i),
    .rst_ni              (rst_ni),
    .scan_cg_en_i        (1'b0),

    .boot_addr_i         (BOOT_ADDR),
    .dm_exception_addr_i (32'h0),
    .dm_halt_addr_i      (32'h0),
    .mhartid_i           (hartid_i),
    .mimpid_patch_i      (4'h0),
    .mtvec_addr_i        (MTVEC_ADDR),

    .instr_req_o         (instr_req_o),
    .instr_gnt_i         (instr_gnt_i),
    .instr_rvalid_i      (instr_rvalid_i),
    .instr_addr_o        (instr_addr_o),
    .instr_memtype_o     (instr_memtype),
    .instr_prot_o        (instr_prot),
    .instr_dbg_o         (instr_dbg),
    .instr_rdata_i       (instr_rdata_i),
    .instr_err_i         (instr_err_i),

    .data_req_o          (data_req_o),
    .data_gnt_i          (data_gnt_i),
    .data_rvalid_i       (data_rvalid_i),
    .data_addr_o         (data_addr_o),
    .data_be_o           (data_be_o),
    .data_we_o           (data_we_o),
    .data_wdata_o        (data_wdata_o),
    .data_memtype_o      (data_memtype),
    .data_prot_o         (data_prot),
    .data_dbg_o          (data_dbg),
    .data_atop_o         (data_atop),
    .data_rdata_i        (data_rdata_i),
    .data_err_i          (data_err_i),
    .data_exokay_i       (1'b1),

    .mcycle_o            (mcycle),
    .time_i              (64'h0),

    .xif_compressed_if   (xif),
    .xif_issue_if        (xif),
    .xif_commit_if       (xif),
    .xif_mem_if          (xif),
    .xif_mem_result_if   (xif),
    .xif_result_if       (xif),

    .irq_i               (32'h0),
    .wu_wfe_i            (1'b0),
    .clic_irq_i          (1'b0),
    .clic_irq_id_i       ('0),
    .clic_irq_level_i    (8'h0),
    .clic_irq_priv_i     (2'h0),
    .clic_irq_shv_i      (1'b0),

    .fencei_flush_req_o  (fencei_flush_req),
    .fencei_flush_ack_i  (fencei_flush_req),

    .debug_req_i         (1'b0),
    .debug_havereset_o   (dbg_havereset),
    .debug_running_o     (dbg_running),
    .debug_halted_o      (dbg_halted),
    .debug_pc_valid_o    (dbg_pc_valid),
    .debug_pc_o          (dbg_pc),

    .fetch_enable_i      (fetch_enable_i),
    .core_sleep_o        (core_sleep_o)
  );

  // The core has no event outputs; tap its controller's mhpmevent bundle. The
  // vendor simulation wrapper adds one hierarchy level (core_i).
`ifdef SYNTHESIS
  assign ev_retire_o      = u_cv32.ctrl_fsm.mhpmevent.minstret;
  assign ev_fetch_stall_o = u_cv32.ctrl_fsm.mhpmevent.if_invalid;
  assign ev_data_stall_o  = u_cv32.ctrl_fsm.mhpmevent.wb_data_stall;
`else
  assign ev_retire_o      = u_cv32.core_i.ctrl_fsm.mhpmevent.minstret;
  assign ev_fetch_stall_o = u_cv32.core_i.ctrl_fsm.mhpmevent.if_invalid;
  assign ev_data_stall_o  = u_cv32.core_i.ctrl_fsm.mhpmevent.wb_data_stall;
`endif

endmodule
