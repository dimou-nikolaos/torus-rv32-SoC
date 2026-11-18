// Network interface unit (SPEC.md, NIU structure; Tile memory map; NoC packet
// format), on both planes: NoC0 (east, south) and NoC1 (west, north).
//
// Plane: every flit goes on the plane select_plane() gives its source and
// destination (fewer hops, ties to NoC0), so flits between two tiles always
// take one path. CMD can force a plane for tests (plane 1: NoC0, 2: NoC1; 3 is
// a command error), which voids that guarantee.
//
// TX-DMA: two channels, each programmed through SRC, DST_ADDR, DST_XY, LEN and
// started by writing CMD = {arg[31:16], doorbell[5], plane[4:3], op[2:0]}.
//   WRITE       reads the scratchpad from SRC in 16-byte words (arbiter port
//               TX-DMA) and injects one WRITE flit per word to DST_XY at
//               DST_ADDR, DST_ADDR + 16, ...; done when the last flit is injected.
//   READ_REQ    channel 0 only: one READ_REQ flit asking DST_XY for LEN bytes
//               from DST_ADDR, returned to SRC here; done when every READ_RSP
//               flit has been written into the scratchpad.
//   ATOMIC_INC  one flit adding arg to the word at DST_ADDR at DST_XY (and
//               ringing its doorbell if bit 5 is set); done when injected.
// Channels interleave flit by flit; each channel's flits leave in address order.
//
// RX, one path per plane: flits leave each ejection buffer strictly in arrival
// order:
//   WRITE, READ_RSP  scratchpad (arbiter port RX0, retired when granted), or
//                    tile control / counter bank (one register write per 32-bit
//                    lane with byte enables set); READ_RSP also counts towards
//                    channel 0's read
//   READ_REQ         into the READ_REQ queue, retired at once (never full: the
//                    depth covers every READ_REQ the grid can have in flight)
//   ATOMIC_INC       read-modify-write on the scratchpad, bank locked for its
//                    two cycles
// Anything else is dropped and counted as an error. The two RX paths share the
// remote register port and the READ_REQ queue (round-robin when both want
// them in the same cycle); atomics from both are kept apart by the bank lock.
//
// TX-reply: serves the READ_REQ queue in order, each request in address order:
// reads the scratchpad (arbiter port TX-reply) or tile control and the counter
// bank (four 32-bit reads per flit on the remote register port, which RX
// writes take first) and sends len/16 READ_RSP flits back on the plane towards
// the requester. A source outside
// those ranges is answered with zeros and counted as an error, so the
// requester never hangs.
//
// Injection: TX-DMA and TX-reply each have a staging FIFO whose entries carry
// their plane; each plane's injection mux serves the heads bound for it,
// round-robin when both are, so DMA and replies on different planes inject in
// the same cycle.
module niu
  import tile_pkg::*;
  import noc_pkg::*;
#(
  parameter int unsigned GX         = 2,
  parameter int unsigned GY         = 2,
  parameter bit          IO_ROW     = 1'b0,  // row GY-1 is the I/O row: only (0, GY-1) has an NIU
  parameter int unsigned DEPTH      = 5,   // router input buffers, and this ejection buffer
  parameter int unsigned STAGE      = 4,   // TX staging buffers between scratchpad and injection
  parameter int unsigned RQ_NEEDED  = 4,   // READ_REQs the whole grid can have in flight
  parameter int unsigned RQ_DEPTH   = RQ_NEEDED
) (
  input  logic     clk_i,
  input  logic     rst_ni,
  input  coord_t   x_i,
  input  coord_t   y_i,
  // NIU registers (local core only)
  input  reg_req_t reg_req_i,
  output reg_rsp_t reg_rsp_o,
  // Scratchpad ports
  output spm_req_t tx_spm_req_o,
  input  spm_rsp_t tx_spm_rsp_i,
  output spm_req_t rep_spm_req_o,
  input  spm_rsp_t rep_spm_rsp_i,
  output spm_req_t rx0_spm_req_o,   // RX of NoC0
  input  spm_rsp_t rx0_spm_rsp_i,
  output spm_req_t rx1_spm_req_o,   // RX of NoC1
  input  spm_rsp_t rx1_spm_rsp_i,
  // Remote register port (tile control, counter bank): answers one cycle later
  output logic        rmt_req_o,
  output logic        rmt_we_o,
  output logic [31:0] rmt_addr_o,
  output logic [3:0]  rmt_be_o,
  output logic [31:0] rmt_wdata_o,
  input  logic        rmt_rvalid_i,
  input  logic        rmt_err_i,
  input  logic [31:0] rmt_rdata_i,
  output logic     doorbell_o,
  // Local ports, per plane (0: NoC0, 1: NoC1)
  output link_t    inj_o        [2],
  input  logic     inj_credit_i [2],
  input  link_t    ej_i         [2],
  output logic     ej_credit_o  [2],
  // Events
  output logic [1:0] ev_tx_flit_o,   // per plane: flit injected
  output logic [1:0] ev_rx_flit_o,   // per plane: flit retired
  output logic     ev_error_o        // a flit dropped, or a READ_REQ answered with zeros
);
  // Every READ_REQ that can be in flight must fit, or RX could stall on the
  // queue and ejection would wait on injection (SPEC.md, NIU structure).
  if (RQ_DEPTH < RQ_NEEDED) begin : g_rq_check
    $fatal(1, "niu: READ_REQ queue depth %0d is below the %0d READ_REQs the grid can have in flight",
           RQ_DEPTH, RQ_NEEDED);
  end

  localparam int unsigned NCH = 2;
  localparam int unsigned CW  = $clog2(DEPTH + 1);
  localparam coord_t      IO_X = '0;
  localparam coord_t      IO_Y = coord_t'(GY - 1);

  // ---------------------------------------------------------------- registers
  typedef struct packed {
    logic [31:0] src, dst_addr, dst_xy, len;
  } cfg_t;

  cfg_t        cfg_q    [NCH];   // as written by software
  cfg_t        act_q    [NCH];   // latched at CMD
  op_e         op_q     [NCH];
  logic [16:0] arg_q    [NCH];   // ATOMIC_INC: {doorbell, increment}
  logic        plane_q  [NCH];   // resolved at CMD
  logic        busy_q   [NCH];
  logic        err_q    [NCH];
  logic [15:0] done_q   [NCH];
  logic [12:0] rd_left_q [NCH];  // flits still to generate (64 KB = 4096)
  logic [6:0]  seq_q    [NCH];
  logic [12:0] rsp_left_q;       // channel 0: READ_RSP flits still to come
  logic [7:0]  rsp_tag_q;        // ... and the tag they carry

  logic       reg_ch;
  logic [4:0] reg_off;
  assign reg_ch  = reg_req_i.addr[5];
  assign reg_off = reg_req_i.addr[4:0];
  logic reg_ok;
  assign reg_ok = reg_req_i.addr[11:6] == '0 && reg_req_i.addr[1:0] == 2'b00;

  // Command checks (SPEC.md: STATUS error bit)
  function automatic logic xy_error(logic [31:0] xy);
    if (xy[31:16] != '0 || xy[15:8] >= 8'(GY) || xy[7:0] >= 8'(GX)) return 1'b1;
    if (IO_ROW && xy[15:8] == 8'(IO_Y) && xy[7:0] != 8'(IO_X)) return 1'b1;  // router-only stop
    return 1'b0;
  endfunction

  // The plane a command's flits take: automatic, or forced by CMD
  function automatic logic cmd_plane(logic [1:0] plane, logic [15:0] xy);
    if (plane == 2'd1) return 1'b0;
    if (plane == 2'd2) return 1'b1;
    return select_plane(x_i, y_i, coord_t'(xy[7:0]), coord_t'(xy[15:8]), GX, GY);
  endfunction

  function automatic logic cmd_error(logic ch, logic [4:0] cmd, cfg_t c, logic busy);
    logic [2:0] op;
    logic [1:0] plane;
    op    = cmd[2:0];
    plane = cmd[4:3];
    if (busy || xy_error(c.dst_xy)) return 1'b1;
    if (plane == 2'd3) return 1'b1;                       // reserved
    unique case (op)
      OP_WRITE, OP_READ_REQ: begin
        if (op == OP_READ_REQ && ch != 1'b0) return 1'b1;  // one read channel per tile
        if (op == OP_READ_REQ && IO_ROW && c.dst_xy[15:8] == 8'(IO_Y)) return 1'b1;  // the I/O tile serves no reads
        if (c.src[3:0] != '0 || c.dst_addr[3:0] != '0 || c.len[3:0] != '0) return 1'b1;
        if (c.len > SPM_BYTES || c.src > SPM_BYTES - c.len) return 1'b1;
        return 1'b0;
      end
      OP_ATOMIC_INC: return c.dst_addr[1:0] != '0;
      default: return 1'b1;
    endcase
  endfunction

  logic [31:0] reg_rdata;
  always_comb begin
    reg_rdata = '0;
    unique case (reg_off)
      5'h00: reg_rdata = cfg_q[reg_ch].src;
      5'h04: reg_rdata = cfg_q[reg_ch].dst_addr;
      5'h08: reg_rdata = cfg_q[reg_ch].dst_xy;
      5'h0C: reg_rdata = cfg_q[reg_ch].len;
      5'h14: reg_rdata = {done_q[reg_ch], 14'h0, err_q[reg_ch], busy_q[reg_ch]};
      default: reg_rdata = '0;
    endcase
  end
  reg_rsp_stage u_rsp (.clk_i, .rst_ni, .req_i(reg_req_i.req), .err_i(!reg_ok || reg_off > 5'h14),
                       .rdata_i(reg_rdata), .rsp_o(reg_rsp_o));

  // ------------------------------------------------- TX-DMA: flit generation
  // One flit per cycle from the selected channel while its staging buffer has
  // room. WRITE flits read the scratchpad (data one cycle after the grant);
  // READ_REQ and ATOMIC_INC flits carry their data from the registers.
  typedef struct packed {
    logic       ch;
    logic       fin;    // the flit that completes its command when injected
    logic       plane;
    flit_t      flit;
  } stage_t;

  logic [$clog2(STAGE):0] st_count;
  logic        gen_q, gen_own_q;   // a flit was generated last cycle; data from the registers
  logic        rd_ch, rd_any, rr_q;

  always_comb begin
    rd_any = 1'b0;
    rd_ch  = 1'b0;
    for (int k = 1; k <= NCH; k++)
      if (!rd_any && busy_q[(int'(rr_q) + k) % NCH] && rd_left_q[(int'(rr_q) + k) % NCH] != '0) begin
        rd_any = 1'b1;
        rd_ch  = 1'((int'(rr_q) + k) % NCH);
      end
  end

  logic st_room, rd_write, gen;
  assign st_room  = (int'(st_count) + int'(gen_q)) < int'(STAGE);
  assign rd_write = op_q[rd_ch] == OP_WRITE;

  logic [12:0] rd_idx;
  assign rd_idx = rd_write ? 13'((act_q[rd_ch].len >> 4) - 32'(rd_left_q[rd_ch])) : '0;

  always_comb begin
    tx_spm_req_o       = '0;
    tx_spm_req_o.req   = rd_any && st_room && rd_write;
    tx_spm_req_o.we    = 1'b0;
    tx_spm_req_o.addr  = SPM_AW'(act_q[rd_ch].src + {rd_idx, 4'h0});
    tx_spm_req_o.be    = '1;
  end
  assign gen = rd_any && st_room && (!rd_write || tx_spm_rsp_i.gnt);

  // Header (and own data) of the flit generated last cycle
  stage_t pend_q;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) gen_q <= 1'b0;
    else         gen_q <= gen;
  always_ff @(posedge clk_i)
    if (gen) begin
      gen_own_q          <= !rd_write;
      pend_q.ch          <= rd_ch;
      pend_q.fin         <= rd_left_q[rd_ch] == 13'd1 && op_q[rd_ch] != OP_READ_REQ;
      pend_q.plane       <= plane_q[rd_ch];
      pend_q.flit.dst_x  <= coord_t'(act_q[rd_ch].dst_xy[7:0]);
      pend_q.flit.dst_y  <= coord_t'(act_q[rd_ch].dst_xy[15:8]);
      pend_q.flit.src_x  <= x_i;
      pend_q.flit.src_y  <= y_i;
      pend_q.flit.op     <= op_q[rd_ch];
      pend_q.flit.vc     <= 1'b0;
      pend_q.flit.addr   <= act_q[rd_ch].dst_addr + {15'h0, rd_idx, 4'h0};
      pend_q.flit.tag    <= {rd_ch, seq_q[rd_ch]};
      unique case (op_q[rd_ch])
        OP_READ_REQ: begin
          pend_q.flit.be   <= '1;
          pend_q.flit.data <= {64'h0, act_q[rd_ch].len, act_q[rd_ch].src};
        end
        OP_ATOMIC_INC: begin
          pend_q.flit.be   <= 16'hF << (4 * act_q[rd_ch].dst_addr[3:2]);
          pend_q.flit.data <= {95'h0, arg_q[rd_ch][16], 16'h0, arg_q[rd_ch][15:0]};
        end
        default: begin
          pend_q.flit.be   <= '1;
          pend_q.flit.data <= '0;
        end
      endcase
    end

  stage_t st_in, st_head;
  logic   st_empty, st_pop;
  always_comb begin
    st_in = pend_q;
    if (!gen_own_q) st_in.flit.data = tx_spm_rsp_i.rdata;
  end
  noc_fifo #(.DEPTH(STAGE), .T(stage_t)) u_stage (
    .clk_i, .rst_ni, .push_i(gen_q), .data_i(st_in), .pop_i(st_pop),
    .empty_o(st_empty), .head_o(st_head), .count_o(st_count)
  );

  // ------------------------------------------------------------------ RX
  typedef enum logic [2:0] {
    RX_SPM_WRITE, RX_REG_WRITE, RX_READ_REQ, RX_ATOMIC, RX_DROP
  } rx_kind_e;
  typedef enum logic [1:0] { SRC_SPM, SRC_REG, SRC_BAD } rq_src_e;
  // READ_REQ queue entry
  typedef struct packed {
    coord_t      src_x, src_y;
    logic [7:0]  tag;
    rq_src_e     src;
    logic [11:0] addr;     // source address [15:4] (registers: 0x1000_xxxx)
    logic [12:0] flits;    // 1..4096
    logic [11:0] ret;      // return address [15:4] at the requester
  } rq_t;
  typedef struct packed { flit_t flit; } ej_t;

  // Per plane, towards the shared parts
  spm_req_t    rx_spm_req [2];
  spm_rsp_t    rx_spm_rsp [2];
  flit_t       rf         [2];      // head flit
  logic        rx_empty   [2];
  logic        rx_pop     [2];
  logic        rx_reg_wr  [2];      // wants the remote register port for a lane write
  logic        reg_gnt    [2];
  logic [31:0] lane_addr  [2];
  logic [3:0]  lane_be    [2];
  logic [31:0] lane_wdata [2];
  logic        rq_req     [2];      // a READ_REQ wants the queue
  logic        rq_gnt     [2];
  rq_t         rq_in_p    [2];
  logic        rx_err     [2];
  logic        rx_bell    [2];
  logic        rsp_retire_p [2];

  assign rx0_spm_req_o = rx_spm_req[0];
  assign rx1_spm_req_o = rx_spm_req[1];
  assign rx_spm_rsp[0] = rx0_spm_rsp_i;
  assign rx_spm_rsp[1] = rx1_spm_rsp_i;

  // READ_RSP is expected only while channel 0 waits for it, with its tag. All
  // replies of one read come from one tile on one plane; if both heads claim
  // the last expected flit, the NoC1 one is the stray and is dropped.
  logic rsp_cand0, rsp_room1;
  assign rsp_cand0 = !rx_empty[0] && rf[0].op == OP_READ_RSP && rf[0].tag == rsp_tag_q &&
                     rf[0].addr[31:SPM_AW] == '0 && rsp_left_q != '0;
  assign rsp_room1 = rsp_left_q > (rsp_cand0 ? 13'd1 : 13'd0);

  for (genvar p = 0; p < 2; p++) begin : g_rx
    ej_t  head;
    logic empty, ej_credit_q;
    logic [$clog2(DEPTH):0] count;
    noc_fifo #(.DEPTH(DEPTH), .T(ej_t)) u_ej (
      .clk_i, .rst_ni, .push_i(ej_i[p].valid), .data_i('{flit: ej_i[p].flit}), .pop_i(rx_pop[p]),
      .empty_o(empty), .head_o(head), .count_o(count)
    );
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) ej_credit_q <= 1'b0;
      else         ej_credit_q <= rx_pop[p];
    assign ej_credit_o[p] = ej_credit_q;
    assign rf[p]       = head.flit;
    assign rx_empty[p] = empty;

    flit_t f;
    assign f = head.flit;

    // Address classes at this tile
    logic f_spm, f_reg;
    assign f_spm = f.addr[31:SPM_AW] == '0;
    assign f_reg = f.addr[31:12] == TCTRL_BASE[31:12] || f.addr[31:12] == PERF_BASE[31:12];

    // READ_REQ fields: data[31:0] return address, data[63:32] length
    logic [31:0] rq_ret, rq_len;
    logic        rq_ok, rq_src_spm, rq_src_reg;
    assign rq_ret     = f.data[31:0];
    assign rq_len     = f.data[63:32];
    assign rq_ok      = rq_len != '0 && rq_len[3:0] == '0 && rq_len <= SPM_BYTES && f.addr[3:0] == '0 &&
                        rq_ret[31:SPM_AW] == '0 && rq_ret[3:0] == '0;
    assign rq_src_spm = f_spm && {1'b0, f.addr} + {1'b0, rq_len} <= 33'(SPM_BYTES);
    assign rq_src_reg = f_reg && 32'(f.addr[11:0]) + rq_len <= 32'd4096;

    logic rsp_ok;
    assign rsp_ok = (p == 0 ? rsp_left_q != '0 : rsp_room1) && f.tag == rsp_tag_q;

    rx_kind_e kind;
    always_comb begin
      kind = RX_DROP;
      unique case (f.op)
        OP_WRITE:      kind = f_spm ? RX_SPM_WRITE : f_reg ? RX_REG_WRITE : RX_DROP;
        OP_READ_RSP:   kind = (f_spm && rsp_ok) ? RX_SPM_WRITE : RX_DROP;
        OP_READ_REQ:   kind = rq_ok ? RX_READ_REQ : RX_DROP;
        OP_ATOMIC_INC: kind = (f_spm && f.addr[1:0] == '0) ? RX_ATOMIC : RX_DROP;
        default:       kind = RX_DROP;
      endcase
    end

    // Register writes: one per 32-bit lane with any byte enable set, lowest first
    logic [3:0] lanes, lanes_done_q, lanes_left;
    logic [1:0] lane;
    logic       lane_last;
    always_comb begin
      for (int l = 0; l < 4; l++) lanes[l] = |f.be[4*l +: 4];
      lanes_left = lanes & ~lanes_done_q;
      lane = '0;
      for (int l = 3; l >= 0; l--) if (lanes_left[l]) lane = 2'(l);
      lane_last = (lanes_left & ~(4'b1 << lane)) == '0;
    end
    assign rx_reg_wr[p]  = !empty && kind == RX_REG_WRITE && lanes_left != '0;
    assign lane_addr[p]  = {f.addr[31:4], lane, 2'b00};
    assign lane_be[p]    = f.be[4 * lane +: 4];
    assign lane_wdata[p] = f.data[32 * lane +: 32];

    // Atomic: phase 0 reads with the lock, phase 1 adds (granted by the lock;
    // the scratchpad adds its read data to the increment)
    logic atom_q;
    always_comb begin
      rx_spm_req[p]       = '0;
      rx_spm_req[p].req   = !empty && (kind == RX_SPM_WRITE || kind == RX_ATOMIC);
      rx_spm_req[p].addr  = f.addr[SPM_AW-1:0];
      if (kind == RX_ATOMIC) begin
        rx_spm_req[p].we    = atom_q;
        rx_spm_req[p].lock  = !atom_q;
        rx_spm_req[p].add   = atom_q;
        rx_spm_req[p].be    = 16'hF << (4 * f.addr[3:2]);
        rx_spm_req[p].wdata = {4{f.data[31:0]}};
      end else begin
        rx_spm_req[p].we    = 1'b1;
        rx_spm_req[p].be    = f.be;
        rx_spm_req[p].wdata = f.data;
      end
    end

    // READ_REQ queue entry
    assign rq_req[p] = !empty && kind == RX_READ_REQ;
    always_comb begin
      rq_in_p[p].src_x = f.src_x;
      rq_in_p[p].src_y = f.src_y;
      rq_in_p[p].tag   = f.tag;
      rq_in_p[p].src   = rq_src_spm ? SRC_SPM : rq_src_reg ? SRC_REG : SRC_BAD;
      rq_in_p[p].addr  = f.addr[15:4];
      rq_in_p[p].flits = 13'(rq_len >> 4);
      rq_in_p[p].ret   = rq_ret[15:4];
    end

    // Retire in order, each kind when its target accepted it
    always_comb begin
      rx_pop[p] = 1'b0;
      if (!empty)
        unique case (kind)
          RX_SPM_WRITE: rx_pop[p] = rx_spm_rsp[p].gnt;
          RX_ATOMIC:    rx_pop[p] = atom_q && rx_spm_rsp[p].gnt;
          RX_REG_WRITE: rx_pop[p] = lanes_left == '0 || (lane_last && reg_gnt[p]);
          RX_READ_REQ:  rx_pop[p] = rq_gnt[p];
          default:      rx_pop[p] = 1'b1;   // drops
        endcase
    end

    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) begin
        atom_q       <= 1'b0;
        lanes_done_q <= '0;
      end else begin
        if (kind == RX_ATOMIC && !empty && rx_spm_rsp[p].gnt) atom_q <= !atom_q;
        lanes_done_q <= rx_pop[p] ? '0 : reg_gnt[p] ? lanes_done_q | (4'b1 << lane) : lanes_done_q;
      end

    assign rx_bell[p]      = rx_pop[p] && kind == RX_ATOMIC && f.data[32];
    assign rx_err[p]       = rx_pop[p] && (kind == RX_DROP || (kind == RX_READ_REQ && rq_in_p[p].src == SRC_BAD));
    assign rsp_retire_p[p] = rx_pop[p] && f.op == OP_READ_RSP && kind == RX_SPM_WRITE;

`ifndef SYNTHESIS
    always_ff @(posedge clk_i or negedge rst_ni)
      if (rst_ni)
        assert (!(kind == RX_ATOMIC && !empty && atom_q && !rx_spm_rsp[p].gnt))
          else $error("niu (%0d,%0d): RX%0d atomic write lost its bank lock", x_i, y_i, p);
`endif

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_rx;
    assign unused_rx = ^{count, ej_i[p].la_port, f.vc, f.dst_x, f.dst_y, f.data[127:64], rq_ret, rq_len,
                         rx_spm_rsp[p].rvalid, rx_spm_rsp[p].rdata};
    /* verilator lint_on UNUSEDSIGNAL */
  end

  // Shared by both RX paths: the remote register port (lane writes) and the
  // READ_REQ queue, round-robin when both ask in the same cycle
  logic reg_rr_q, rq_rr_q;
  assign reg_gnt[0] = rx_reg_wr[0] && (!rx_reg_wr[1] || reg_rr_q);
  assign reg_gnt[1] = rx_reg_wr[1] && !reg_gnt[0];
  assign rq_gnt[0]  = rq_req[0] && (!rq_req[1] || rq_rr_q);
  assign rq_gnt[1]  = rq_req[1] && !rq_gnt[0];
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      reg_rr_q <= 1'b0;
      rq_rr_q  <= 1'b0;
    end else begin
      if (reg_gnt[0] || reg_gnt[1]) reg_rr_q <= reg_gnt[1];   // 1: RX0 next
      if (rq_gnt[0] || rq_gnt[1])   rq_rr_q  <= rq_gnt[1];
    end

  logic rx_reg_any;
  assign rx_reg_any = rx_reg_wr[0] || rx_reg_wr[1];

  // READ_REQ queue
  rq_t  rq_in, rq_head;
  logic rq_push, rq_pop, rq_empty;
  logic [$clog2(RQ_DEPTH):0] rq_count;
  assign rq_push = rq_gnt[0] || rq_gnt[1];
  assign rq_in   = rq_gnt[1] ? rq_in_p[1] : rq_in_p[0];
  noc_fifo #(.DEPTH(RQ_DEPTH), .T(rq_t)) u_rq (
    .clk_i, .rst_ni, .push_i(rq_push), .data_i(rq_in), .pop_i(rq_pop),
    .empty_o(rq_empty), .head_o(rq_head), .count_o(rq_count)
  );

  assign doorbell_o   = rx_bell[0] || rx_bell[1];
  assign ev_rx_flit_o = {rx_pop[1], rx_pop[0]};

  // ------------------------------------------------------------- TX-reply
  typedef struct packed { logic plane; flit_t flit; } rstage_t;
  logic       rp_active_q;
  logic       rp_plane_q;      // towards the requester
  rq_t        rp_q;
  logic [12:0] rp_left_q;      // flits still to generate
  logic [15:0] rp_off_q;       // byte offset of the next flit
  logic       rp_spm_q;        // scratchpad read in flight
  logic       rp_asm_q;        // register flit being assembled
  logic [1:0] rp_word_q;       // next register word to read
  logic [1:0] rp_rword_q;      // word whose answer arrives this cycle
  logic       rp_rd_q;         // a register read is in flight
  logic [127:0] rp_buf_q;
  flit_t      rp_pend_q;
  logic [$clog2(STAGE):0] rs_count;

  logic rp_room;
  assign rp_room = (int'(rs_count) + int'(rp_spm_q) + int'(rp_asm_q)) < int'(STAGE);

  // Source of the next flit; a register flit's words 1..3 come after its header
  // advanced the offset
  logic [31:0] rp_src_addr, rp_reg_addr;
  assign rp_src_addr = (rp_q.src == SRC_REG ? 32'h1000_0000 : 32'h0) + {16'h0, rp_q.addr, 4'h0} + 32'(rp_off_q);
  assign rp_reg_addr = rp_src_addr - (rp_word_q != '0 ? 32'd16 : 32'd0);

  // This cycle's step of the reply engine
  logic rp_spm_req, rp_spm_gnt, rp_bad_gen, rp_reg_rd, rp_hdr;
  assign rp_spm_req = rp_active_q && rp_q.src == SRC_SPM && rp_left_q != '0 && rp_room;
  assign rp_spm_gnt = rp_spm_req && rep_spm_rsp_i.gnt;
  assign rp_bad_gen = rp_active_q && rp_q.src == SRC_BAD && rp_left_q != '0 && rp_room;
  // Register reads yield to RX register writes
  assign rp_reg_rd  = rp_active_q && rp_q.src == SRC_REG && !rx_reg_any &&
                      (rp_word_q != '0 || (rp_left_q != '0 && rp_room));
  assign rp_hdr     = rp_spm_gnt || rp_bad_gen || (rp_reg_rd && rp_word_q == '0);

  always_comb begin
    rep_spm_req_o      = '0;
    rep_spm_req_o.req  = rp_spm_req;
    rep_spm_req_o.addr = SPM_AW'(rp_src_addr);
    rep_spm_req_o.be   = '1;
  end

  flit_t rp_hdr_flit;
  always_comb begin
    rp_hdr_flit       = '0;
    rp_hdr_flit.dst_x = rp_q.src_x;
    rp_hdr_flit.dst_y = rp_q.src_y;
    rp_hdr_flit.src_x = x_i;
    rp_hdr_flit.src_y = y_i;
    rp_hdr_flit.op    = OP_READ_RSP;
    rp_hdr_flit.addr  = {16'h0, rp_q.ret, 4'h0} + 32'(rp_off_q);
    rp_hdr_flit.be    = '1;
    rp_hdr_flit.tag   = rp_q.tag;
  end

  logic rp_done;
  assign rp_done = rp_active_q && rp_left_q == '0 && !rp_spm_q && !rp_asm_q;
  assign rq_pop  = !rp_active_q && !rq_empty;

  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) begin
      rp_active_q <= 1'b0;
      rp_spm_q    <= 1'b0;
      rp_asm_q    <= 1'b0;
      rp_word_q   <= '0;
      rp_rd_q     <= 1'b0;
      rp_left_q   <= '0;
      rp_off_q    <= '0;
    end else begin
      if (rq_pop) begin
        rp_active_q <= 1'b1;
        rp_left_q   <= rq_head.flits;
        rp_off_q    <= '0;
      end else if (rp_done) begin
        rp_active_q <= 1'b0;
      end
      if (rp_hdr) begin
        rp_left_q <= rp_left_q - 1'b1;
        rp_off_q  <= rp_off_q + 16'd16;
      end
      rp_spm_q <= rp_spm_gnt;
      rp_rd_q  <= rp_reg_rd;
      if (rp_reg_rd) rp_word_q <= rp_word_q + 1'b1;
      // A flit's last word can arrive in the cycle the next flit's first is read
      rp_asm_q <= (rp_asm_q && !(rp_rd_q && rp_rword_q == 2'd3)) || (rp_reg_rd && rp_word_q == '0);
    end

  always_ff @(posedge clk_i) begin
    if (rq_pop) begin
      rp_q       <= rq_head;
      rp_plane_q <= select_plane(x_i, y_i, rq_head.src_x, rq_head.src_y, GX, GY);
    end
    if (rp_hdr)    rp_pend_q <= rp_hdr_flit;
    if (rp_reg_rd) rp_rword_q <= rp_word_q;
    if (rp_rd_q)   rp_buf_q[32 * rp_rword_q +: 32] <= rmt_err_i ? '0 : rmt_rdata_i;
  end

  // Staging: scratchpad data one cycle after the grant, registers after the
  // fourth word, zeros at once
  rstage_t rs_in, rs_head;
  logic    rs_push, rs_empty, rs_pop;
  always_comb begin
    rs_in.plane = rp_plane_q;
    rs_in.flit  = rp_pend_q;
    rs_push     = 1'b0;
    if (rp_spm_q) begin
      rs_push         = 1'b1;
      rs_in.flit.data = rep_spm_rsp_i.rdata;
    end else if (rp_rd_q && rp_rword_q == 2'd3) begin
      rs_push         = 1'b1;
      rs_in.flit.data = {rmt_err_i ? 32'h0 : rmt_rdata_i, rp_buf_q[95:0]};
    end else if (rp_bad_gen) begin
      rs_push         = 1'b1;
      rs_in.flit      = rp_hdr_flit;
      rs_in.flit.data = '0;
    end
  end
  noc_fifo #(.DEPTH(STAGE), .T(rstage_t)) u_rstage (
    .clk_i, .rst_ni, .push_i(rs_push), .data_i(rs_in), .pop_i(rs_pop),
    .empty_o(rs_empty), .head_o(rs_head), .count_o(rs_count)
  );

  // Remote register port: RX lane writes, else reply-engine reads
  logic rx_wsel;   // which RX path writes
  assign rx_wsel     = reg_gnt[1];
  assign rmt_req_o   = rx_reg_any || rp_reg_rd;
  assign rmt_we_o    = rx_reg_any;
  assign rmt_addr_o  = rx_reg_any ? lane_addr[rx_wsel] : {rp_reg_addr[31:4], rp_word_q, 2'b00};
  assign rmt_be_o    = rx_reg_any ? lane_be[rx_wsel] : 4'hF;
  assign rmt_wdata_o = lane_wdata[rx_wsel];

  // Errors: dropped flits, and READ_REQs answered with zeros (cycles with one)
  assign ev_error_o = rx_err[0] || rx_err[1];

  // -------------------------------------------------------- injection mux
  // Per plane: the staging heads bound for it, round-robin (inj_rr_q: the last
  // flit sent was a reply)
  logic inj_send [2], inj_rep [2];
  for (genvar p = 0; p < 2; p++) begin : g_inj
    logic [CW-1:0] credits;
    logic          inj_rr_q, cand_st, cand_rs;
    link_t         q;
    flit_t         fl;
    assign cand_st    = !st_empty && st_head.plane == 1'(p);
    assign cand_rs    = !rs_empty && rs_head.plane == 1'(p);
    assign inj_rep[p] = cand_rs && (!cand_st || !inj_rr_q);
    assign inj_send[p] = (cand_st || cand_rs) && credits != '0;
    assign fl         = inj_rep[p] ? rs_head.flit : st_head.flit;

    credit_counter #(.DEPTH(DEPTH)) u_cc (
      .clk_i, .rst_ni, .send_i(inj_send[p]), .credit_i(inj_credit_i[p]), .credits_o(credits)
    );
    always_ff @(posedge clk_i or negedge rst_ni)
      if (!rst_ni) begin
        q.valid  <= 1'b0;
        inj_rr_q <= 1'b0;
      end else begin
        q.valid <= inj_send[p];
        if (inj_send[p]) inj_rr_q <= inj_rep[p];
      end
    always_ff @(posedge clk_i)
      if (inj_send[p]) begin
        q.flit    <= fl;
        q.la_port <= route(x_i, y_i, fl.dst_x, fl.dst_y);
      end
    assign inj_o[p]        = q;
    assign ev_tx_flit_o[p] = inj_send[p];
  end
  assign st_pop = (inj_send[0] && !inj_rep[0]) || (inj_send[1] && !inj_rep[1]);
  assign rs_pop = (inj_send[0] && inj_rep[0]) || (inj_send[1] && inj_rep[1]);

  // ------------------------------------------------- channel state machine
  logic cmd_wr;
  assign cmd_wr = reg_req_i.req && reg_req_i.we && reg_ok && reg_off == 5'h10;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      for (int c = 0; c < NCH; c++) begin
        cfg_q[c]      <= '0;
        act_q[c]      <= '0;
        plane_q[c]    <= 1'b0;
        op_q[c]       <= OP_WRITE;
        arg_q[c]      <= '0;
        busy_q[c]     <= 1'b0;
        err_q[c]      <= 1'b0;
        done_q[c]     <= '0;
        rd_left_q[c]  <= '0;
        seq_q[c]      <= '0;
      end
      rsp_left_q <= '0;
      rsp_tag_q  <= '0;
      rr_q       <= 1'b0;
    end else begin
      // Configuration writes
      if (reg_req_i.req && reg_req_i.we && reg_ok)
        unique case (reg_off)
          5'h00: cfg_q[reg_ch].src      <= reg_req_i.wdata;
          5'h04: cfg_q[reg_ch].dst_addr <= reg_req_i.wdata;
          5'h08: cfg_q[reg_ch].dst_xy   <= reg_req_i.wdata;
          5'h0C: cfg_q[reg_ch].len      <= reg_req_i.wdata;
          default: ;
        endcase
      // Command start
      if (cmd_wr) begin
        if (cmd_error(reg_ch, reg_req_i.wdata[4:0], cfg_q[reg_ch], busy_q[reg_ch])) begin
          err_q[reg_ch]  <= 1'b1;
          if (!busy_q[reg_ch]) done_q[reg_ch] <= done_q[reg_ch] + 1'b1;  // never hangs dma_wait
        end else if (reg_req_i.wdata[2:0] != OP_ATOMIC_INC && cfg_q[reg_ch].len == '0) begin
          err_q[reg_ch]  <= 1'b0;
          done_q[reg_ch] <= done_q[reg_ch] + 1'b1;
        end else begin
          err_q[reg_ch]      <= 1'b0;
          act_q[reg_ch]      <= cfg_q[reg_ch];
          plane_q[reg_ch]    <= cmd_plane(reg_req_i.wdata[4:3], cfg_q[reg_ch].dst_xy[15:0]);
          op_q[reg_ch]       <= op_e'(reg_req_i.wdata[2:0]);
          arg_q[reg_ch]      <= {reg_req_i.wdata[5], reg_req_i.wdata[31:16]};
          busy_q[reg_ch]     <= 1'b1;
          rd_left_q[reg_ch]  <= reg_req_i.wdata[2:0] == OP_WRITE ? 13'(cfg_q[reg_ch].len >> 4) : 13'd1;
          if (reg_req_i.wdata[2:0] == OP_READ_REQ) begin
            rsp_left_q <= 13'(cfg_q[reg_ch].len >> 4);
            rsp_tag_q  <= {reg_ch, seq_q[reg_ch]};
          end
        end
      end
      // Flits generated
      if (gen) begin
        rd_left_q[rd_ch] <= rd_left_q[rd_ch] - 1'b1;
        seq_q[rd_ch]     <= seq_q[rd_ch] + 1'b1;
        rr_q             <= rd_ch;
      end
      // WRITE and ATOMIC_INC complete with their last flit injected
      if (st_pop && st_head.fin) begin
        busy_q[st_head.ch] <= 1'b0;
        done_q[st_head.ch] <= done_q[st_head.ch] + 1'b1;
      end
      // READ_REQ completes with its last reply flit written (one per plane at
      // most per cycle; rsp_room1 keeps the sum within what is left)
      if (rsp_retire_p[0] || rsp_retire_p[1]) begin
        rsp_left_q <= rsp_left_q - 13'(rsp_retire_p[0]) - 13'(rsp_retire_p[1]);
        if (rsp_left_q == 13'(rsp_retire_p[0]) + 13'(rsp_retire_p[1])) begin
          busy_q[0] <= 1'b0;
          done_q[0] <= done_q[0] + 1'b1;
        end
      end
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni)
    if (rst_ni) begin
      assert (!(rq_push && int'(rq_count) == int'(RQ_DEPTH)))
        else $error("niu (%0d,%0d): READ_REQ queue overflow", x_i, y_i);
      assert (!(rx_reg_any && rp_reg_rd)) else $error("niu: register port conflict");
    end
`endif

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{reg_req_i.be, tx_spm_rsp_i.rvalid, rep_spm_rsp_i.rvalid,
                    rmt_rvalid_i, act_q[0].dst_xy[31:16], act_q[1].dst_xy[31:16], reg_req_i.wdata[15:6],
                    rp_src_addr[31:16], rp_reg_addr[3:0], rp_buf_q[127:96], rp_q.flits, rf[1]};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
