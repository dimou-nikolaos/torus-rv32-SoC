// AXI-to-NoC bridge of the I/O tile, with its network interface (SPEC.md, I/O
// tile), in the fabric clock domain.
//
// Requests come from the crossing in AXI order:
//   write beat  -> one WRITE flit to (x, y, local address) with the beat's
//                  strobes as byte enables (none set: nothing to send); to the
//                  I/O tile's own register block, written directly
//   read burst  -> split into chunks of up to RD_CHUNK_BEATS beats, each taking
//                  one of OUT reorder slots: a READ_REQ whose replies return to
//                  slot * 64 here, or for the I/O tile's own registers (and
//                  bursts the front-end refused) a slot filled locally
// Slots drain in allocation order into the response crossing, so read beats
// return in request order whatever order the tiles answer in; at most OUT
// READ_REQs are in flight (IO_RD_OUTSTANDING in the grid's queue sizing).
// Writes and reads leave in arrival order, so a read after a write to the
// same tile sees it (path ordering).
//
// The network interface sits on both planes. Each flit goes on the plane
// select_plane() gives the I/O tile and its destination, so everything to one
// tile takes one path. Ejection drains both planes, one flit per cycle,
// round-robin when both have one: READ_RSP into its slot, WRITE and ATOMIC_INC
// into the register block, anything else dropped and counted. A plane's head
// waits at most one cycle for the other, never for injection.
module io_bridge
  import noc_pkg::*;
  import io_pkg::*;
#(
  parameter int unsigned GX    = 2,
  parameter int unsigned GY    = 3,
  parameter int unsigned DEPTH = 5,
  parameter int unsigned OUT   = 8
) (
  input  logic         clk_i,
  input  logic         rst_ni,
  input  coord_t       x_i,
  input  coord_t       y_i,
  // Request crossing (read side)
  input  io_req_t      req_i,
  input  logic         req_empty_i,
  output logic         req_pop_o,
  // Response crossing (write side)
  output logic         rsp_push_o,
  output io_rsp_t      rsp_o,
  input  logic         rsp_full_i,
  // NoC local ports, per plane (0: NoC0, 1: NoC1)
  output link_t        inj_o        [2],
  input  logic         inj_credit_i [2],
  input  link_t        ej_i         [2],
  output logic         ej_credit_o  [2],
  // Register block
  output logic         host_we_o,
  output logic [11:0]  host_addr_o,
  output logic [15:0]  host_strb_o,
  output logic [127:0] host_wdata_o,
  output logic [11:0]  host_raddr_o,
  input  logic [127:0] host_rdata_i,
  output logic         rx_we_o,
  output logic [11:0]  rx_addr_o,
  output logic [15:0]  rx_be_o,
  output logic [127:0] rx_wdata_o,
  output logic         rx_inc_o,
  output logic [11:0]  rx_inc_addr_o,
  output logic [31:0]  rx_inc_val_o,
  output logic         ev_wr_flit_o,
  output logic         ev_rd_req_o,
  output logic         ev_rsp_flit_o,
  output logic         ev_rx_err_o,
  output logic         ev_rx_flit_o
);
  localparam int unsigned SW = $clog2(OUT);
  localparam int unsigned CW = $clog2(DEPTH + 1);
  localparam int unsigned CB = RD_CHUNK_BEATS;
  if (OUT < 1 || OUT > 16 || (1 << SW) != OUT) begin : g_out_check
    $fatal(1, "io_bridge: OUT %0d must be a power of two up to 16", OUT);
  end
  if (CB != 4) begin : g_chunk_check
    $fatal(1, "io_bridge: slots hold four beats (64-byte return areas)");
  end

  // ------------------------------------------------------------ request head
  io_req_t h;
  assign h = req_i;
  coord_t  h_x, h_y;
  logic    h_io;
  assign h_x  = h.addr[31:28];
  assign h_y  = h.addr[27:24];
  assign h_io = h_x == x_i && h_y == y_i;

  // Local address at the target of the beat at global address a
  /* verilator lint_off UNUSEDSIGNAL */
  function automatic logic [31:0] local_addr(logic [31:0] a);
    return a[20] ? {16'h1000, a[15:0]} : {16'h0, a[15:0]};
  endfunction
  /* verilator lint_on UNUSEDSIGNAL */

  // ------------------------------------------------------------- injection
  // The head's plane; one flit per cycle on it when it has a credit
  logic          h_plane;
  logic          inj_send;
  logic          can_send [2];
  flit_t         inj_flit;
  assign h_plane = select_plane(x_i, y_i, h_x, h_y, GX, GY);
  for (genvar p = 0; p < 2; p++) begin : g_inj
    logic [CW-1:0] credits;
    logic          send;
    link_t         q;
    assign send        = inj_send && h_plane == 1'(p);
    assign can_send[p] = credits != '0;
    credit_counter #(.DEPTH(DEPTH)) u_cc (
      .clk_i, .rst_ni, .send_i(send), .credit_i(inj_credit_i[p]), .credits_o(credits)
    );
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) q.valid <= 1'b0;
      else         q.valid <= send;
    always_ff @(posedge clk_i)
      if (send) begin
        q.flit    <= inj_flit;
        q.la_port <= route(x_i, y_i, inj_flit.dst_x, inj_flit.dst_y);
      end
    assign inj_o[p] = q;
  end

  // ------------------------------------------------------------ reorder slots
  typedef enum logic [1:0] { SLOT_REMOTE, SLOT_LOCAL, SLOT_ERR } slot_kind_e;
  typedef struct packed {
    slot_kind_e         kind;
    logic [2:0]         beats;    // 1..4
    logic               last;     // holds the burst's last beat
    logic [AXI_IDW-1:0] id;
    logic [11:0]        addr;     // local: register block offset of the first beat
  } meta_t;

  meta_t        meta_q [OUT];
  logic [2:0]   recv_q [OUT];
  logic [127:0] buf_q  [OUT][CB];
  logic [SW-1:0] alloc_q, drain_q;
  logic [SW:0]   used_q;
  logic [1:0]    dbeat_q;

  // -------------------------------------------------- request processing
  logic [8:0] rd_off_q, rd_total, rd_left;
  logic [2:0] chunk;
  logic [31:0] chunk_addr;
  assign rd_total   = {1'b0, h.len} + 9'd1;
  assign rd_left    = rd_total - rd_off_q;
  assign chunk      = rd_left >= 9'(CB) ? 3'(CB) : 3'(rd_left);
  assign chunk_addr = h.addr + {19'h0, rd_off_q, 4'h0};

  logic slot_free, can_inj;
  assign slot_free = int'(used_q) < int'(OUT);
  assign can_inj   = can_send[h_plane];

  logic wr_head, rd_head, wr_local, wr_send, rd_issue, rd_send, drain_free;
  assign wr_head  = !req_empty_i && !h.rd;
  assign rd_head  = !req_empty_i && h.rd;
  assign wr_local = wr_head && h_io;
  assign wr_send  = wr_head && !h_io && h.strb != '0 && can_inj;
  assign rd_issue = rd_head && slot_free && (h.err || h_io || can_inj);
  assign rd_send  = rd_issue && !h.err && !h_io;

  assign req_pop_o = (wr_head && (h_io || h.strb == '0 || can_inj)) ||
                     (rd_issue && rd_off_q + 9'(chunk) == rd_total);

  assign inj_send = wr_send || rd_send;
  always_comb begin
    inj_flit       = '0;
    inj_flit.dst_x = h_x;
    inj_flit.dst_y = h_y;
    inj_flit.src_x = x_i;
    inj_flit.src_y = y_i;
    if (rd_send) begin
      inj_flit.op   = OP_READ_REQ;
      inj_flit.addr = local_addr(chunk_addr);
      inj_flit.be   = '1;
      inj_flit.tag  = 8'(alloc_q);
      inj_flit.data = {64'h0, 25'h0, chunk, 4'h0, 16'h0, 10'(alloc_q), 6'h0};  // length, return address
    end else begin
      inj_flit.op   = OP_WRITE;
      inj_flit.addr = local_addr(h.addr);
      inj_flit.be   = h.strb;
      inj_flit.data = h.data;
    end
  end

  assign host_we_o    = wr_local;
  assign host_addr_o  = h.addr[11:0];
  assign host_strb_o  = h.strb;
  assign host_wdata_o = h.data;

  assign ev_wr_flit_o = wr_send;
  assign ev_rd_req_o  = rd_send;

  // ------------------------------------------------------------------ drain
  meta_t dm;
  assign dm = meta_q[drain_q];
  logic d_ready, d_push, d_slot_done;
  assign d_ready     = used_q != '0 && (dm.kind != SLOT_REMOTE || recv_q[drain_q] == dm.beats);
  assign d_push      = d_ready && !rsp_full_i;
  assign d_slot_done = d_push && 3'(dbeat_q) == dm.beats - 3'd1;
  assign drain_free  = d_slot_done;

  assign host_raddr_o = dm.addr + {6'h0, dbeat_q, 4'h0};
  always_comb begin
    rsp_o      = '0;
    rsp_o.id   = dm.id;
    rsp_o.last = dm.last && d_slot_done;
    rsp_o.err  = dm.kind == SLOT_ERR;
    unique case (dm.kind)
      SLOT_REMOTE: rsp_o.data = buf_q[drain_q][dbeat_q];
      SLOT_LOCAL:  rsp_o.data = host_rdata_i;
      default:     rsp_o.data = '0;
    endcase
  end
  assign rsp_push_o = d_push;

  // ------------------------------------------------------------------- RX
  // One ejection buffer per plane; the drain takes one head per cycle
  typedef struct packed { flit_t flit; } ej_t;
  ej_t  rx_head [2];
  logic rx_empty [2], rx_pop [2];
  logic rx_sel, rx_rr_q, rx_any;
  for (genvar p = 0; p < 2; p++) begin : g_ej
    logic ej_credit_q;
    logic [$clog2(DEPTH):0] count;
    noc_fifo #(.DEPTH(DEPTH), .T(ej_t)) u_ej (
      .clk_i, .rst_ni, .push_i(ej_i[p].valid), .data_i('{flit: ej_i[p].flit}), .pop_i(rx_pop[p]),
      .empty_o(rx_empty[p]), .head_o(rx_head[p]), .count_o(count)
    );
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) ej_credit_q <= 1'b0;
      else         ej_credit_q <= rx_pop[p];
    assign ej_credit_o[p] = ej_credit_q;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_ej;
    assign unused_ej = ^{count, ej_i[p].la_port};
    /* verilator lint_on UNUSEDSIGNAL */
  end
  // NoC1 when only it has a flit, or both have and NoC0 went last
  assign rx_sel    = !rx_empty[1] && (rx_empty[0] || rx_rr_q);
  assign rx_any    = !rx_empty[0] || !rx_empty[1];
  assign rx_pop[0] = rx_any && !rx_sel;   // ejection never waits
  assign rx_pop[1] = rx_any && rx_sel;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni)     rx_rr_q <= 1'b0;
    else if (rx_any) rx_rr_q <= !rx_sel;

  flit_t rf;
  assign rf = rx_head[rx_sel].flit;
  logic [SW-1:0] rf_slot;
  logic [1:0]    rf_beat;
  logic          rf_page, rsp_ok;
  assign rf_slot = rf.addr[6 +: SW];
  assign rf_beat = rf.addr[5:4];
  assign rf_page = rf.addr[31:12] == IO_REG_BASE[31:12];
  // A reply must land in a waiting remote slot, within its beats, with its tag
  assign rsp_ok  = rf.addr[31:6+SW] == '0 && rf.tag == 8'(rf_slot) && meta_q[rf_slot].kind == SLOT_REMOTE &&
                   slot_used(rf_slot) && 3'(rf_beat) < meta_q[rf_slot].beats && recv_q[rf_slot] < meta_q[rf_slot].beats;

  // Slot s is allocated (between drain and alloc pointers)
  function automatic logic slot_used(logic [SW-1:0] s);
    logic [SW:0] d;
    d = (SW + 1)'(SW'(s - drain_q));   // distance from the drain pointer, modulo OUT
    return d < used_q;
  endfunction

  logic rx_rsp, rx_wr, rx_atom;
  assign rx_rsp  = rx_any && rf.op == OP_READ_RSP && rsp_ok;
  assign rx_wr   = rx_any && rf.op == OP_WRITE && rf_page;
  assign rx_atom = rx_any && rf.op == OP_ATOMIC_INC && rf_page && rf.addr[1:0] == '0;

  assign rx_we_o       = rx_wr;
  assign rx_addr_o     = rf.addr[11:0];
  assign rx_be_o       = rf.be;
  assign rx_wdata_o    = rf.data;
  assign rx_inc_o      = rx_atom;
  assign rx_inc_addr_o = rf.addr[11:0];
  assign rx_inc_val_o  = rf.data[31:0];
  assign ev_rsp_flit_o = rx_rsp;
  assign ev_rx_err_o   = rx_any && !rx_rsp && !rx_wr && !rx_atom;
  assign ev_rx_flit_o  = rx_any;

  // ------------------------------------------------------------------ state
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      rd_off_q <= '0;
      alloc_q  <= '0;
      drain_q  <= '0;
      used_q   <= '0;
      dbeat_q  <= '0;
      for (int s = 0; s < OUT; s++) begin
        recv_q[s] <= '0;
        meta_q[s] <= meta_t'('0);
      end
    end else begin
      if (rd_issue) begin
        rd_off_q <= (rd_off_q + 9'(chunk) == rd_total) ? '0 : rd_off_q + 9'(chunk);
        alloc_q  <= alloc_q + 1'b1;
        meta_q[alloc_q] <= '{kind:  h.err ? SLOT_ERR : h_io ? SLOT_LOCAL : SLOT_REMOTE,
                             beats: chunk,
                             last:  rd_off_q + 9'(chunk) == rd_total,
                             id:    h.id,
                             addr:  chunk_addr[11:0]};
        recv_q[alloc_q] <= '0;
      end
      used_q <= used_q + (SW + 1)'(rd_issue) - (SW + 1)'(drain_free);
      if (d_push) dbeat_q <= d_slot_done ? '0 : dbeat_q + 1'b1;
      if (d_slot_done) drain_q <= drain_q + 1'b1;
      if (rx_rsp) recv_q[rf_slot] <= recv_q[rf_slot] + 1'b1;
    end

  always_ff @(posedge clk_i)
    if (rx_rsp) buf_q[rf_slot][rf_beat] <= rf.data;

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni) begin
      assert (!(rx_any && rf.op == OP_READ_RSP && !rsp_ok))
        else $error("io_bridge: READ_RSP to %08x tag %02x matches no waiting slot", rf.addr, rf.tag);
      assert (!(rd_issue && rx_rsp && rf_slot == alloc_q)) else $error("io_bridge: reply to a slot being allocated");
    end
`endif

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{rf.vc, rf.dst_x, rf.dst_y, rf.src_x, rf.src_y, chunk_addr[31:12],
                    h.addr[19:16], h.addr[23:21], rf.data[127:32]};
  /* verilator lint_on UNUSEDSIGNAL */
endmodule
