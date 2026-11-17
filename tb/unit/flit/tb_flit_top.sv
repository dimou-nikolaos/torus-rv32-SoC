// Packs flit fields with noc_pkg::flit_t so the C++ mirror can be checked.
module tb_flit_top
  import noc_pkg::*;
(
  input  logic [3:0]   dst_x_i, dst_y_i, src_x_i, src_y_i,
  input  logic [2:0]   op_i,
  input  logic         vc_i,
  input  logic [31:0]  addr_i,
  input  logic [15:0]  be_i,
  input  logic [7:0]   tag_i,
  input  logic [127:0] data_i,
  output flit_t        flit_o,
  output logic [15:0]  bits_o
);
  assign flit_o = '{dst_x: dst_x_i, dst_y: dst_y_i, src_x: src_x_i, src_y: src_y_i,
                    op: op_e'(op_i), vc: vc_i, addr: addr_i, be: be_i, tag: tag_i, data: data_i};
  assign bits_o = 16'(FLIT_W);
endmodule
