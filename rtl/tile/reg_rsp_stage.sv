// Helper: one register-port response stage. Captures read data computed
// combinationally by the owner and returns it one cycle after the request.
module reg_rsp_stage
  import tile_pkg::*;
(
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic        req_i,
  input  logic        err_i,
  input  logic [31:0] rdata_i,
  output reg_rsp_t    rsp_o
);
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) rsp_o <= '0;
    else begin
      rsp_o.rvalid <= req_i;
      rsp_o.err    <= req_i && err_i;
      rsp_o.rdata  <= req_i ? rdata_i : '0;
    end
endmodule
