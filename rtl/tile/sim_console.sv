// Console and exit registers (simulation only). The testbench implements the
// DPI functions: it owns stdout formatting and the exit code. In synthesis the
// block accepts and drops writes.
module sim_console
  import tile_pkg::*;
(
  input  logic     clk_i,
  input  logic     rst_ni,
  input  logic [3:0] coord_x_i,
  input  logic [3:0] coord_y_i,
  input  reg_req_t req_i,
  output reg_rsp_t rsp_o
);
`ifndef SYNTHESIS
  import "DPI-C" function void torus_console_putc(input int x, input int y, input byte c);
  import "DPI-C" function void torus_sim_exit(input int x, input int y, input int code);

  always_ff @(posedge clk_i)
    if (req_i.req && req_i.we) begin
      if (req_i.addr == 12'(CONSOLE_TX))   torus_console_putc(int'(coord_x_i), int'(coord_y_i), req_i.wdata[7:0]);
      if (req_i.addr == 12'(CONSOLE_EXIT)) torus_sim_exit(int'(coord_x_i), int'(coord_y_i), req_i.wdata);
    end
`endif

  logic err;
  assign err = !req_i.we || !(req_i.addr inside {12'(CONSOLE_TX), 12'(CONSOLE_EXIT)});
  reg_rsp_stage u_rsp (.clk_i, .rst_ni, .req_i(req_i.req), .err_i(err), .rdata_i('0), .rsp_o(rsp_o));

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{req_i.be, coord_x_i, coord_y_i, req_i.wdata};
  // rst_ni is used only by the response stage; the core is held in reset
  // whenever this block's reset is asserted, so no request can arrive then.
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
