// Tile control registers (SPEC.md, Tile memory map): COORD, CTRL, CYCLE,
// DOORBELL, GRID. Two register ports: the local core and the remote side (the
// NIU). On a same-cycle write to the same register the remote side wins.
module tile_ctrl
  import tile_pkg::*;
#(
  parameter int unsigned NX   = 1,   // compute grid, for GRID
  parameter int unsigned NY   = 1,
  parameter int unsigned IO_X = 0,   // the I/O tile
  parameter int unsigned IO_Y = 1
) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [3:0]  coord_x_i,
  input  logic [3:0]  coord_y_i,
  input  reg_req_t    core_req_i,
  output reg_rsp_t    core_rsp_o,
  input  reg_req_t    rmt_req_i,
  output reg_rsp_t    rmt_rsp_o,
  input  logic        doorbell_set_i,   // from the NIU (ATOMIC_INC with the doorbell flag)
  output logic        run_o,
  output logic [63:0] cycle_o
);
  logic        run_q, doorbell_q;
  logic [63:0] cycle_q;
  logic [31:0] hi_shadow_q [2];

  reg_req_t req [2];
  assign req[0] = core_req_i;
  assign req[1] = rmt_req_i;

  function automatic logic wr(logic v, logic we, logic [11:0] addr, logic be0, logic [7:0] off);
    return v && we && addr == 12'(off) && be0;
  endfunction

  logic [31:0] rdata [2];
  logic        err   [2];
  always_comb
    for (int p = 0; p < 2; p++) begin
      err[p]   = 1'b0;
      rdata[p] = '0;
      unique case (req[p].addr)
        12'(TCTRL_COORD):    rdata[p] = {16'h0, 4'h0, coord_y_i, 4'h0, coord_x_i};
        12'(TCTRL_CTRL):     rdata[p] = {31'h0, run_q};
        12'(TCTRL_CYCLE_LO): rdata[p] = cycle_q[31:0];
        12'(TCTRL_CYCLE_HI): rdata[p] = hi_shadow_q[p];
        12'(TCTRL_DOORBELL): rdata[p] = {31'h0, doorbell_q};
        12'(TCTRL_GRID):     rdata[p] = {8'(IO_Y), 8'(IO_X), 8'(NY), 8'(NX)};
        default:             err[p]   = 1'b1;
      endcase
      if (req[p].we && req[p].addr inside {12'(TCTRL_COORD), 12'(TCTRL_CYCLE_LO), 12'(TCTRL_CYCLE_HI), 12'(TCTRL_GRID)})
        err[p] = 1'b1;   // read-only
    end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      run_q       <= 1'b0;
      doorbell_q  <= 1'b0;
      cycle_q     <= '0;
      hi_shadow_q <= '{default: '0};
    end else begin
      cycle_q <= cycle_q + 1;
      for (int p = 0; p < 2; p++) begin
        if (wr(req[p].req, req[p].we, req[p].addr, req[p].be[0], TCTRL_CTRL)) run_q <= req[p].wdata[0];
        if (req[p].req && !req[p].we && req[p].addr == 12'(TCTRL_CYCLE_LO))
          hi_shadow_q[p] <= cycle_q[63:32];
      end
      if (doorbell_set_i)
        doorbell_q <= 1'b1;
      else if (wr(req[0].req, req[0].we, req[0].addr, req[0].be[0], TCTRL_DOORBELL) && req[0].wdata[0] ||
               wr(req[1].req, req[1].we, req[1].addr, req[1].be[0], TCTRL_DOORBELL) && req[1].wdata[0])
        doorbell_q <= 1'b0;
    end
  end

  assign run_o   = run_q;
  assign cycle_o = cycle_q;

  reg_rsp_stage u_core_rsp (.clk_i, .rst_ni, .req_i(core_req_i.req), .err_i(err[0]), .rdata_i(rdata[0]), .rsp_o(core_rsp_o));
  reg_rsp_stage u_rmt_rsp  (.clk_i, .rst_ni, .req_i(rmt_req_i.req),  .err_i(err[1]), .rdata_i(rdata[1]), .rsp_o(rmt_rsp_o));

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{core_req_i.wdata[31:1], rmt_req_i.wdata[31:1], core_req_i.be[3:1], rmt_req_i.be[3:1]};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
