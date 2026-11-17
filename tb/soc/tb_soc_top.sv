// SoC testbench top: the torus of tiles plus one RVFI monitor per tile that
// hands every retired instruction to the C++ lockstep checker of that tile.
module tb_soc_top #(
  parameter int unsigned NX         = 1,
  parameter int unsigned NY         = 1,
  parameter int unsigned LINK_SLICE = 0,
  localparam int unsigned N         = NX * NY
) (
  input  logic              clk_i,
  input  logic              rst_ni,
  input  logic [N-1:0]      tb_req_i,
  input  logic [N-1:0]      tb_we_i,
  input  logic [N-1:0][31:0] tb_addr_i,
  input  logic [N-1:0][31:0] tb_wdata_i,
  output logic [N-1:0]      tb_rvalid_o,
  output logic [N-1:0]      tb_err_o,
  output logic [N-1:0][31:0] tb_rdata_o
);
  soc_top #(.NX(NX), .NY(NY), .LINK_SLICE(LINK_SLICE)) u_soc (.*);

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
