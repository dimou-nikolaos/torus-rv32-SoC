// SoC testbench top: the SoC (tiles, I/O row, I/O tile with its AXI port) plus
// one RVFI monitor per tile that hands every retired instruction to the C++
// lockstep checker of that tile.
module tb_soc_top #(
  parameter int unsigned NX         = 1,
  parameter int unsigned NY         = 1,
  parameter int unsigned LINK_SLICE = 0
) (
  input  logic          clk_i,
  input  logic          rst_ni,
  input  logic          aclk_i,
  input  logic          arst_ni,
  input  logic [3:0]    s_axi_awid_i,
  input  logic [31:0]   s_axi_awaddr_i,
  input  logic [7:0]    s_axi_awlen_i,
  input  logic [2:0]    s_axi_awsize_i,
  input  logic [1:0]    s_axi_awburst_i,
  input  logic          s_axi_awvalid_i,
  output logic          s_axi_awready_o,
  input  logic [127:0]  s_axi_wdata_i,
  input  logic [15:0]   s_axi_wstrb_i,
  input  logic          s_axi_wlast_i,
  input  logic          s_axi_wvalid_i,
  output logic          s_axi_wready_o,
  output logic [3:0]    s_axi_bid_o,
  output logic [1:0]    s_axi_bresp_o,
  output logic          s_axi_bvalid_o,
  input  logic          s_axi_bready_i,
  input  logic [3:0]    s_axi_arid_i,
  input  logic [31:0]   s_axi_araddr_i,
  input  logic [7:0]    s_axi_arlen_i,
  input  logic [2:0]    s_axi_arsize_i,
  input  logic [1:0]    s_axi_arburst_i,
  input  logic          s_axi_arvalid_i,
  output logic          s_axi_arready_o,
  output logic [3:0]    s_axi_rid_o,
  output logic [127:0]  s_axi_rdata_o,
  output logic [1:0]    s_axi_rresp_o,
  output logic          s_axi_rlast_o,
  output logic          s_axi_rvalid_o,
  input  logic          s_axi_rready_i
);
  soc_top #(.NX(NX), .NY(NY), .LINK_SLICE(LINK_SLICE), .IDW(4)) u_soc (.*);

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

  for (genvar y = 0; y < NY; y++) begin : g_my
    for (genvar x = 0; x < NX; x++) begin : g_mx
      // RVFI lives in the vendor simulation wrapper (rtl/core/core_wrap.sv).
      // Only while the core runs; the C++ side also ignores the initialization
      // trigger of --x-initial-edge.
      `define RVFI u_soc.g_y[y].g_x[x].g_tile.u_tile.u_core.u_cv32.rvfi_i
      always_ff @(posedge clk_i)
        if (u_soc.g_y[y].g_x[x].g_tile.u_tile.run && `RVFI.rvfi_valid)
          torus_rvfi_retire(
            y * 16 + x,
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
    end
  end
endmodule
