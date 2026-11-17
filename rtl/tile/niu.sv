// Network interface unit, M1 subset (SPEC.md, NIU structure; Tile memory map).
//
// TX-DMA: two channels, each programmed through SRC, DST_ADDR, DST_XY, LEN and
// started by writing CMD. A WRITE command reads the scratchpad from SRC in 16-byte
// words (arbiter port TX-DMA) and injects one WRITE flit per word to DST_XY at
// DST_ADDR, DST_ADDR + 16, ... Channels interleave flit by flit; each channel's
// flits leave in address order. A command completes when its last flit has been
// injected (SPEC.md, Ordering and completion, rule 4).
//
// RX: flits leave the ejection buffer strictly in arrival order. A WRITE to the
// scratchpad goes to arbiter port RX0 and is retired when granted; a WRITE to
// tile control or the counter bank goes to the remote register port.
//
// Not yet (M2): READ_REQ with its reply engine and queue, ATOMIC_INC, NoC1.
// Commands that need them complete at once with STATUS.error set; such flits
// arriving at RX are dropped and counted as errors.
module niu
  import tile_pkg::*;
  import noc_pkg::*;
#(
  parameter int unsigned GX         = 2,
  parameter int unsigned GY         = 2,
  parameter bit          IO_ROW     = 1'b0,  // row GY-1 is the I/O row: only (0, GY-1) has an NIU
  parameter int unsigned DEPTH      = 5,   // router input buffers, and this ejection buffer
  parameter int unsigned STAGE      = 4    // TX staging buffer between scratchpad and injection
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
  output spm_req_t rx_spm_req_o,
  input  spm_rsp_t rx_spm_rsp_i,
  // Remote register access (tile control, counter bank)
  output logic        rmt_req_o,
  output logic [31:0] rmt_addr_o,
  output logic [31:0] rmt_wdata_o,
  // NoC0 local port
  output link_t    inj_o,
  input  logic     inj_credit_i,
  input  link_t    ej_i,
  output logic     ej_credit_o,
  // Events
  output logic     ev_tx_flit_o,
  output logic     ev_rx_flit_o,
  output logic     ev_error_o
);
  localparam int unsigned NCH = 2;
  localparam int unsigned CW  = $clog2(DEPTH + 1);

  // ---------------------------------------------------------------- registers
  typedef struct packed {
    logic [31:0] src, dst_addr, dst_xy, len;
  } cfg_t;

  cfg_t        cfg_q    [NCH];   // as written by software
  cfg_t        act_q    [NCH];   // latched at CMD
  logic        busy_q   [NCH];
  logic        err_q    [NCH];
  logic [15:0] done_q   [NCH];
  logic [12:0] rd_left_q [NCH];  // flits still to read from the scratchpad (64 KB = 4096)
  logic [12:0] inj_left_q [NCH]; // flits still to inject
  logic [6:0]  seq_q    [NCH];

  logic       reg_ch;
  logic [4:0] reg_off;
  assign reg_ch  = reg_req_i.addr[5];
  assign reg_off = reg_req_i.addr[4:0];
  logic reg_ok;
  assign reg_ok = reg_req_i.addr[11:6] == '0 && reg_req_i.addr[1:0] == 2'b00;

  // Command checks (SPEC.md: STATUS error bit)
  function automatic logic cmd_error(int unsigned ch, logic [4:0] cmd, cfg_t c, logic busy);
    logic [2:0] op;
    logic [1:0] plane;
    op    = cmd[2:0];
    plane = cmd[4:3];
    if (busy) return 1'b1;
    if (op != OP_WRITE) return 1'b1;                     // READ_REQ, ATOMIC_INC: M2
    if (plane == 2'd2 || plane == 2'd3) return 1'b1;     // NoC1: M3
    if (c.src[3:0] != '0 || c.dst_addr[3:0] != '0 || c.len[3:0] != '0) return 1'b1;
    if (c.len > SPM_BYTES || c.src > SPM_BYTES - c.len) return 1'b1;
    if (c.dst_xy[31:16] != '0 || c.dst_xy[15:8] >= 8'(GY) || c.dst_xy[7:0] >= 8'(GX)) return 1'b1;
    if (IO_ROW && c.dst_xy[15:8] == 8'(GY - 1) && c.dst_xy[7:0] != '0) return 1'b1;  // router-only stop
    return ch > NCH;  // never
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

  // ------------------------------------------------- TX: scratchpad reads
  // Requests one word per cycle from the selected channel while the staging
  // buffer has room for it; read data arrives one cycle after the grant.
  typedef struct packed {
    logic       ch;
    logic       last;
    flit_t      flit;
  } stage_t;

  logic        st_full_room;
  logic [$clog2(STAGE):0] st_count;
  logic        rd_inflight_q;
  logic        rd_ch;            // channel chosen this cycle
  logic        rd_any;
  logic        rr_q;

  always_comb begin
    rd_any = 1'b0;
    rd_ch  = 1'b0;
    for (int k = 1; k <= NCH; k++)
      if (!rd_any && busy_q[(int'(rr_q) + k) % NCH] && rd_left_q[(int'(rr_q) + k) % NCH] != '0) begin
        rd_any = 1'b1;
        rd_ch  = 1'((int'(rr_q) + k) % NCH);
      end
  end
  assign st_full_room = (int'(st_count) + int'(rd_inflight_q)) < int'(STAGE);

  logic [12:0] rd_idx;
  assign rd_idx = 13'((act_q[rd_ch].len >> 4) - 32'(rd_left_q[rd_ch]));

  always_comb begin
    tx_spm_req_o       = '0;
    tx_spm_req_o.req   = rd_any && st_full_room;
    tx_spm_req_o.we    = 1'b0;
    tx_spm_req_o.addr  = SPM_AW'(act_q[rd_ch].src + {rd_idx, 4'h0});
    tx_spm_req_o.be    = '1;
  end
  logic rd_gnt;
  assign rd_gnt = tx_spm_req_o.req && tx_spm_rsp_i.gnt;

  // Header of the read in flight, completed with the data when it returns
  stage_t pend_q;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) rd_inflight_q <= 1'b0;
    else         rd_inflight_q <= rd_gnt;
  always_ff @(posedge clk_i)
    if (rd_gnt) begin
      pend_q.ch          <= rd_ch;
      pend_q.last        <= rd_left_q[rd_ch] == 13'd1;
      pend_q.flit.dst_x  <= coord_t'(act_q[rd_ch].dst_xy[7:0]);
      pend_q.flit.dst_y  <= coord_t'(act_q[rd_ch].dst_xy[15:8]);
      pend_q.flit.src_x  <= x_i;
      pend_q.flit.src_y  <= y_i;
      pend_q.flit.op     <= OP_WRITE;
      pend_q.flit.vc     <= 1'b0;
      pend_q.flit.addr   <= act_q[rd_ch].dst_addr + {15'h0, rd_idx, 4'h0};
      pend_q.flit.be     <= '1;
      pend_q.flit.tag    <= {rd_ch, seq_q[rd_ch]};
      pend_q.flit.data   <= '0;
    end

  stage_t st_in, st_head;
  logic   st_empty, st_pop;
  always_comb begin
    st_in           = pend_q;
    st_in.flit.data = tx_spm_rsp_i.rdata;
  end
  noc_fifo #(.DEPTH(STAGE), .T(stage_t)) u_stage (
    .clk_i, .rst_ni, .push_i(rd_inflight_q), .data_i(st_in), .pop_i(st_pop),
    .empty_o(st_empty), .head_o(st_head), .count_o(st_count)
  );

  // ----------------------------------------------------------- TX: inject
  logic [CW-1:0] inj_credits;
  assign st_pop = !st_empty && inj_credits != '0;
  credit_counter #(.DEPTH(DEPTH)) u_cc (
    .clk_i, .rst_ni, .send_i(st_pop), .credit_i(inj_credit_i), .credits_o(inj_credits)
  );
  link_t inj_q;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) inj_q.valid <= 1'b0;
    else         inj_q.valid <= st_pop;
  always_ff @(posedge clk_i)
    if (st_pop) begin
      inj_q.flit    <= st_head.flit;
      inj_q.la_port <= route(x_i, y_i, st_head.flit.dst_x, st_head.flit.dst_y);
    end
  assign inj_o        = inj_q;
  assign ev_tx_flit_o = st_pop;

  // ------------------------------------------------- channel state machine
  logic cmd_wr;
  assign cmd_wr = reg_req_i.req && reg_req_i.we && reg_ok && reg_off == 5'h10;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      for (int c = 0; c < NCH; c++) begin
        cfg_q[c]      <= '0;
        act_q[c]      <= '0;
        busy_q[c]     <= 1'b0;
        err_q[c]      <= 1'b0;
        done_q[c]     <= '0;
        rd_left_q[c]  <= '0;
        inj_left_q[c] <= '0;
        seq_q[c]      <= '0;
      end
      rr_q <= 1'b0;
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
        if (cmd_error(int'(reg_ch), reg_req_i.wdata[4:0], cfg_q[reg_ch], busy_q[reg_ch])) begin
          err_q[reg_ch]  <= 1'b1;
          if (!busy_q[reg_ch]) done_q[reg_ch] <= done_q[reg_ch] + 1'b1;  // never hangs dma_wait
        end else if (cfg_q[reg_ch].len == '0) begin
          err_q[reg_ch]  <= 1'b0;
          done_q[reg_ch] <= done_q[reg_ch] + 1'b1;
        end else begin
          err_q[reg_ch]      <= 1'b0;
          act_q[reg_ch]      <= cfg_q[reg_ch];
          busy_q[reg_ch]     <= 1'b1;
          rd_left_q[reg_ch]  <= 13'(cfg_q[reg_ch].len >> 4);
          inj_left_q[reg_ch] <= 13'(cfg_q[reg_ch].len >> 4);
        end
      end
      // Reads granted
      if (rd_gnt) begin
        rd_left_q[rd_ch] <= rd_left_q[rd_ch] - 1'b1;
        seq_q[rd_ch]     <= seq_q[rd_ch] + 1'b1;
        rr_q             <= rd_ch;
      end
      // Flits injected; the last one completes the command
      if (st_pop) begin
        inj_left_q[st_head.ch] <= inj_left_q[st_head.ch] - 1'b1;
        if (st_head.last) begin
          busy_q[st_head.ch] <= 1'b0;
          done_q[st_head.ch] <= done_q[st_head.ch] + 1'b1;
        end
      end
    end
  end

  // ------------------------------------------------------------------ RX
  typedef struct packed { flit_t flit; } ej_t;
  ej_t  rx_head;
  logic rx_empty, rx_pop, ej_credit_q;
  logic [$clog2(DEPTH):0] rx_count;
  noc_fifo #(.DEPTH(DEPTH), .T(ej_t)) u_ej (
    .clk_i, .rst_ni, .push_i(ej_i.valid), .data_i('{flit: ej_i.flit}), .pop_i(rx_pop),
    .empty_o(rx_empty), .head_o(rx_head), .count_o(rx_count)
  );
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) ej_credit_q <= 1'b0;
    else         ej_credit_q <= rx_pop;
  assign ej_credit_o = ej_credit_q;

  flit_t rf;
  assign rf = rx_head.flit;
  logic rx_write, rx_spm, rx_reg;
  assign rx_write = !rx_empty && rf.op == OP_WRITE;
  assign rx_spm   = rx_write && rf.addr[31:SPM_AW] == '0;
  assign rx_reg   = rx_write && (rf.addr[31:12] == TCTRL_BASE[31:12] || rf.addr[31:12] == PERF_BASE[31:12]);

  always_comb begin
    rx_spm_req_o       = '0;
    rx_spm_req_o.req   = rx_spm;
    rx_spm_req_o.we    = 1'b1;
    rx_spm_req_o.addr  = rf.addr[SPM_AW-1:0];
    rx_spm_req_o.be    = rf.be;
    rx_spm_req_o.wdata = rf.data;
  end

  // A register write takes the 32-bit lane named by addr[3:2]
  assign rmt_req_o   = rx_reg;
  assign rmt_addr_o  = rf.addr;
  assign rmt_wdata_o = rf.data[32 * rf.addr[3:2] +: 32];

  // Retire in order: scratchpad writes when granted, everything else at once
  assign rx_pop       = !rx_empty && (!rx_spm || rx_spm_rsp_i.gnt);
  assign ev_rx_flit_o = rx_pop;
  assign ev_error_o   = rx_pop && !rx_spm && !rx_reg;

  /* verilator lint_off UNUSEDSIGNAL */
  logic unused;
  assign unused = ^{reg_req_i.be, tx_spm_rsp_i.rvalid, rx_spm_rsp_i.rvalid, rx_spm_rsp_i.rdata,
                    rx_count, ej_i.la_port, inj_left_q[0], inj_left_q[1], rf.src_x, rf.src_y, rf.vc,
                    rf.tag, rf.dst_x, rf.dst_y, act_q[0].dst_xy[31:16], act_q[1].dst_xy[31:16],
                    reg_req_i.wdata[31:5]};
  /* verilator lint_on UNUSEDSIGNAL */

endmodule
