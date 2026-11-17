// I/O tile register block (SPEC.md, I/O tile), at local 0x1000_4000 of the
// I/O tile. Host semaphores that kernels increment with ATOMIC_INC (SEM[0] is
// the DONE counter of the launch protocol), and the I/O tile's counters. The
// host reaches it through the bridge without crossing the NoC; tiles reach it
// with WRITE and ATOMIC_INC flits.
//
//   0x000 + 4k  SEM[k], k < IO_NSEM   read/write, ATOMIC_INC target
//   0x100       INFO      RO {IO_RD_OUTSTANDING, GY, GX, version}
//   0x104       CYCLES    RO fabric cycles, low 32 bits
//   0x108..     counters  RO WRITE flits injected, READ_REQs injected, READ_RSP
//                         flits received, flits dropped, atomics executed, all
//                         flits received
// Other offsets read as zero and ignore writes.
//
// Same-cycle updates of one semaphore compose: a NoC write replaces a host
// write, and an atomic adds to the result.
module io_regs
  import io_pkg::*;
#(
  parameter int unsigned GX  = 2,
  parameter int unsigned GY  = 3,
  parameter int unsigned OUT = 8
) (
  input  logic         clk_i,
  input  logic         rst_ni,
  // Host side (bridge): one 16-byte beat
  input  logic         host_we_i,
  input  logic [11:0]  host_addr_i,    // beat address in the page
  input  logic [15:0]  host_strb_i,
  input  logic [127:0] host_wdata_i,
  output logic [127:0] host_rdata_o,   // combinational, beat at host_raddr_i
  input  logic [11:0]  host_raddr_i,
  // NoC side: WRITE and ATOMIC_INC flits
  input  logic         rx_we_i,
  input  logic [11:0]  rx_addr_i,
  input  logic [15:0]  rx_be_i,
  input  logic [127:0] rx_wdata_i,
  input  logic         rx_inc_i,
  input  logic [11:0]  rx_inc_addr_i,  // word address
  input  logic [31:0]  rx_inc_val_i,
  // Events
  input  logic         ev_wr_flit_i,
  input  logic         ev_rd_req_i,
  input  logic         ev_rsp_flit_i,
  input  logic         ev_rx_err_i,
  input  logic         ev_rx_flit_i
);
  localparam int unsigned NCNT = 6;   // WR_FLITS, RD_REQS, RSP_FLITS, RX_ERRORS, ATOMICS, RX_FLITS

  logic [31:0] sem_q  [IO_NSEM];
  logic [31:0] cnt_q  [NCNT];
  logic [31:0] cyc_q;

  function automatic logic [31:0] merge(logic [31:0] cur, logic [31:0] wd, logic [3:0] be);
    logic [31:0] r;
    r = cur;
    for (int b = 0; b < 4; b++) if (be[b]) r[8*b +: 8] = wd[8*b +: 8];
    return r;
  endfunction

  // A beat at `base` covers words base/4 .. base/4 + 3
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic hits(logic [11:0] base, int unsigned k);
    return base[11:4] == 8'(k / 4);
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      sem_q <= '{default: '0};
      cnt_q <= '{default: '0};
      cyc_q <= '0;
    end else begin
      for (int k = 0; k < IO_NSEM; k++) begin
        logic [31:0] v;
        v = sem_q[k];
        if (host_we_i && hits(host_addr_i, k)) v = merge(v, host_wdata_i[32*(k%4) +: 32], host_strb_i[4*(k%4) +: 4]);
        if (rx_we_i && hits(rx_addr_i, k))     v = merge(v, rx_wdata_i[32*(k%4) +: 32], rx_be_i[4*(k%4) +: 4]);
        if (rx_inc_i && rx_inc_addr_i[11:2] == 10'(k)) v = v + rx_inc_val_i;
        sem_q[k] <= v;
      end
      cyc_q <= cyc_q + 1'b1;
      if (ev_wr_flit_i)  cnt_q[0] <= cnt_q[0] + 1'b1;
      if (ev_rd_req_i)   cnt_q[1] <= cnt_q[1] + 1'b1;
      if (ev_rsp_flit_i) cnt_q[2] <= cnt_q[2] + 1'b1;
      if (ev_rx_err_i)   cnt_q[3] <= cnt_q[3] + 1'b1;
      if (rx_inc_i)      cnt_q[4] <= cnt_q[4] + 1'b1;
      if (ev_rx_flit_i)  cnt_q[5] <= cnt_q[5] + 1'b1;
    end

  function automatic logic [31:0] word(logic [9:0] w);
    logic [11:0] a;
    a = {w, 2'b00};
    if (int'(w) < IO_NSEM) return sem_q[w[$clog2(IO_NSEM)-1:0]];
    unique case (a)
      IO_INFO:      return {8'(OUT), 8'(GY), 8'(GX), IO_VERSION};
      IO_CYCLES:    return cyc_q;
      IO_WR_FLITS:  return cnt_q[0];
      IO_RD_REQS:   return cnt_q[1];
      IO_RSP_FLITS: return cnt_q[2];
      IO_RX_ERRORS: return cnt_q[3];
      IO_ATOMICS:   return cnt_q[4];
      IO_RX_FLITS:  return cnt_q[5];
      default:      return '0;
    endcase
  endfunction

  always_comb
    for (int l = 0; l < 4; l++) host_rdata_o[32*l +: 32] = word({host_raddr_i[11:4], 2'(l)});

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{host_addr_i[3:0], rx_addr_i[3:0], host_raddr_i[3:0], rx_inc_addr_i[1:0]};
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
