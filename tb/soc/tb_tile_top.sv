// Single-tile testbench top (M0): the tile plus an RVFI monitor that hands every
// retired instruction to the C++ lockstep checker through DPI.
module tb_tile_top (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [3:0]  coord_x_i,
  input  logic [3:0]  coord_y_i,
  input  logic        rmt_req_i,
  input  logic        rmt_we_i,
  input  logic [31:0] rmt_addr_i,
  input  logic [31:0] rmt_wdata_i,
  output logic        rmt_rvalid_o,
  output logic        rmt_err_o,
  output logic [31:0] rmt_rdata_o
);
  tile_top u_tile (.*);

  import "DPI-C" function void torus_rvfi_retire(
    input int     hart,
    input longint order,
    input int     pc_rdata,
    input int     pc_wdata,
    input int     insn,
    input bit     trap,
    input int     exc_cause,
    input bit     intr,
    input int     intr_cause,
    input int     rd_addr,
    input int     rd_wdata,
    input int     mem_addr,
    input int     mem_rmask,
    input int     mem_wmask,
    input int     mem_rdata,
    input int     mem_wdata,
    input bit     mem_err);

  // RVFI lives in the vendor simulation wrapper (rtl/core/core_wrap.sv)
  `define RVFI u_tile.u_core.u_cv32.rvfi_i
  // Only while the core runs; the C++ side also ignores the initialization
  // trigger of --x-initial-edge.
  always_ff @(posedge clk_i)
    if (u_tile.run && `RVFI.rvfi_valid)
      torus_rvfi_retire(
        int'({coord_y_i, coord_x_i}),
        `RVFI.rvfi_order,
        `RVFI.rvfi_pc_rdata,
        `RVFI.rvfi_pc_wdata,
        `RVFI.rvfi_insn,
        `RVFI.rvfi_trap.trap,
        int'(`RVFI.rvfi_trap.exception_cause),
        `RVFI.rvfi_intr.intr,
        int'(`RVFI.rvfi_intr.cause),
        int'(`RVFI.rvfi_rd_addr),
        `RVFI.rvfi_rd_wdata,
        `RVFI.rvfi_mem_addr[31:0],
        int'(`RVFI.rvfi_mem_rmask[3:0]),
        int'(`RVFI.rvfi_mem_wmask[3:0]),
        `RVFI.rvfi_mem_rdata[31:0],
        `RVFI.rvfi_mem_wdata[31:0],
        `RVFI.rvfi_mem_err[0]);
  `undef RVFI
endmodule
