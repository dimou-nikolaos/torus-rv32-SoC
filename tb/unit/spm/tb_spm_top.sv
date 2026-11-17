// Flattens the spm_banks port arrays into packed vectors for the C++ testbench.
module tb_spm_top
  import tile_pkg::*;
#(
  parameter int unsigned STARVE_LIMIT = 4
) (
  input  logic                              clk_i,
  input  logic                              rst_ni,
  input  logic [SPM_PORTS-1:0]              req_i,
  input  logic [SPM_PORTS-1:0]              we_i,
  input  logic [SPM_PORTS-1:0]              lock_i,
  input  logic [SPM_PORTS-1:0][SPM_AW-1:0]  addr_i,
  input  logic [SPM_PORTS-1:0][15:0]        be_i,
  input  logic [SPM_PORTS-1:0][127:0]       wdata_i,
  output logic [SPM_PORTS-1:0]              gnt_o,
  output logic [SPM_PORTS-1:0]              rvalid_o,
  output logic [SPM_PORTS-1:0][127:0]       rdata_o,
  output logic [SPM_PORTS-1:0]              denied_o,
  output logic [SPM_PORTS-1:0]              promoted_o
);
  spm_req_t req [SPM_PORTS];
  spm_rsp_t rsp [SPM_PORTS];

  always_comb
    for (int p = 0; p < SPM_PORTS; p++) begin
      req[p]      = '{req: req_i[p], we: we_i[p], lock: lock_i[p], addr: addr_i[p],
                      be: be_i[p], wdata: wdata_i[p]};
      gnt_o[p]    = rsp[p].gnt;
      rvalid_o[p] = rsp[p].rvalid;
      rdata_o[p]  = rsp[p].rdata;
    end

  spm_banks #(.STARVE_LIMIT(STARVE_LIMIT)) u_dut (
    .clk_i, .rst_ni, .req_i(req), .rsp_o(rsp), .denied_o, .promoted_o
  );
endmodule
