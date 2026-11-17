// 32-bit OBI master (core instruction or data port) to one 128-bit scratchpad
// port. Byte address bits [3:2] select the 32-bit lane. Fully pipelined: the
// scratchpad answers exactly one cycle after each grant, so OBI order holds.
module obi_spm_adapter
  import tile_pkg::*;
(
  input  logic        clk_i,
  input  logic        rst_ni,
  // OBI slave
  input  logic        req_i,
  output logic        gnt_o,
  input  logic [31:0] addr_i,
  input  logic        we_i,
  input  logic [3:0]  be_i,
  input  logic [31:0] wdata_i,
  output logic        rvalid_o,
  output logic [31:0] rdata_o,
  // Scratchpad master
  output spm_req_t    spm_req_o,
  input  spm_rsp_t    spm_rsp_i
);
  logic [1:0] lane_q;

  always_comb begin
    spm_req_o.req   = req_i;
    spm_req_o.we    = we_i;
    spm_req_o.lock  = 1'b0;
    spm_req_o.add   = 1'b0;
    spm_req_o.addr  = addr_i[SPM_AW-1:0];
    spm_req_o.be    = 16'(be_i) << (4 * addr_i[3:2]);
    spm_req_o.wdata = {4{wdata_i}};
  end

  assign gnt_o    = spm_rsp_i.gnt;
  assign rvalid_o = spm_rsp_i.rvalid;
  assign rdata_o  = spm_rsp_i.rdata[32*lane_q +: 32];

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni)                     lane_q <= '0;
    else if (req_i && spm_rsp_i.gnt) lane_q <= addr_i[3:2];

  // addr_i bits above the scratchpad are decoded by the caller
  /* verilator lint_off UNUSEDSIGNAL */
  logic [31-SPM_AW:0] unused_addr;
  assign unused_addr = addr_i[31:SPM_AW];
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
