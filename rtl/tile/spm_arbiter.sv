// Bank arbiter: priority with a starvation bound (SPEC.md, Scratchpad bank arbiter).
//
//   1. Lock: a port granted with `lock` owns the bank for the next cycle if it
//      requests again (the write half of an atomic read-modify-write).
//   2. Promoted ports (denied STARVE_LIMIT consecutive cycles) win over all
//      default priorities; several promoted ports are served round-robin.
//   3. Default order: engine; the NIU's TX ports (TX-DMA, TX-reply), round-
//      robin; its RX ports (RX0, RX1), round-robin; core data; core
//      instruction. TX before RX: a read a TX stream loses is a cycle its
//      flits never get back, and the gap it leaves in the receiver's stream
//      shifts that stream back into bank conflicts; a write an RX stream loses
//      waits in the ejection buffer's spare slot and the streams fall out of
//      phase for good.
//
// Pure arbitration: one-hot grant per cycle, no data path.
module spm_arbiter
  import tile_pkg::*;
#(
  parameter int unsigned NPORTS       = SPM_PORTS,
  parameter int unsigned STARVE_LIMIT = 4,
  localparam int unsigned PW          = $clog2(NPORTS),
  localparam int unsigned AGEW        = $clog2(STARVE_LIMIT + 1)
) (
  input  logic              clk_i,
  input  logic              rst_ni,
  input  logic [NPORTS-1:0] req_i,
  input  logic [NPORTS-1:0] lock_i,
  output logic [NPORTS-1:0] gnt_o,
  // Events for the counter bank
  output logic [NPORTS-1:0] denied_o,     // requested, not granted this cycle
  output logic [NPORTS-1:0] promoted_o    // granted through promotion
);
  initial begin
    assert (STARVE_LIMIT >= 1) else $fatal(1, "STARVE_LIMIT must be >= 1");
    assert (NPORTS == SPM_PORTS) else $fatal(1, "the default order names the tile's %0d ports", SPM_PORTS);
  end

  logic [AGEW-1:0]   age_q [NPORTS];
  logic              lock_q;
  logic [PW-1:0]     lock_port_q;
  logic [PW-1:0]     rr_prom_q, rr_tx_q, rr_rx_q;   // last winner of each round-robin
  logic [NPORTS-1:0] promoted;

  always_comb
    for (int i = 0; i < NPORTS; i++)
      promoted[i] = req_i[i] && (age_q[i] >= AGEW'(STARVE_LIMIT));

  // Round-robin pick in [lo, hi]: first set bit strictly after `last`, wrapping.
  function automatic logic [NPORTS-1:0] rr_pick(logic [NPORTS-1:0] cand,
                                                int unsigned lo, int unsigned hi,
                                                logic [PW-1:0] last);
    logic [NPORTS-1:0] g;
    int unsigned n;
    /* verilator lint_off UNUSEDSIGNAL */
    int          idx;   // only the low bits index `cand`
    /* verilator lint_on UNUSEDSIGNAL */
    g = '0;
    n = hi - lo + 1;
    for (int unsigned k = 1; k <= n; k++) begin
      idx = int'(lo + ((int'(last) - lo + k) % n));
      if (g == '0 && cand[idx]) g[idx] = 1'b1;
    end
    return g;
  endfunction

  logic [NPORTS-1:0] tx_mask, rx_mask;
  always_comb begin
    tx_mask = '0;
    rx_mask = '0;
    tx_mask[PORT_TXDMA] = 1'b1;
    tx_mask[PORT_TXREP] = 1'b1;
    rx_mask[PORT_RX0]   = 1'b1;
    rx_mask[PORT_RX1]   = 1'b1;
  end

  logic use_prom, use_lock;
  assign use_lock = lock_q && req_i[lock_port_q];
  always_comb begin
    gnt_o    = '0;
    use_prom = 1'b0;
    if (use_lock) begin
      gnt_o[lock_port_q] = 1'b1;
    end else if (|promoted) begin
      use_prom = 1'b1;
      gnt_o    = rr_pick(promoted, 0, NPORTS - 1, rr_prom_q);
    end else if (req_i[PORT_ENGINE]) begin
      gnt_o[PORT_ENGINE] = 1'b1;
    end else if (|(req_i & tx_mask)) begin
      gnt_o = rr_pick(req_i & tx_mask, PORT_TXDMA, PORT_TXREP, rr_tx_q);
    end else if (|(req_i & rx_mask)) begin
      gnt_o = rr_pick(req_i & rx_mask, PORT_RX0, PORT_RX1, rr_rx_q);
    end else if (req_i[PORT_CORE_D]) begin
      gnt_o[PORT_CORE_D] = 1'b1;
    end else if (req_i[PORT_CORE_I]) begin
      gnt_o[PORT_CORE_I] = 1'b1;
    end
  end

  assign denied_o   = req_i & ~gnt_o;
  assign promoted_o = use_prom ? gnt_o : '0;

  function automatic logic [PW-1:0] idx_of(logic [NPORTS-1:0] oh);
    logic [PW-1:0] r;
    r = '0;
    for (int i = 0; i < NPORTS; i++) if (oh[i]) r = PW'(i);
    return r;
  endfunction

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      lock_q      <= 1'b0;
      lock_port_q <= '0;
      rr_prom_q   <= PW'(NPORTS - 1);
      rr_tx_q     <= PW'(PORT_TXREP);
      rr_rx_q     <= PW'(PORT_RX1);
      for (int i = 0; i < NPORTS; i++) age_q[i] <= '0;
    end else begin
      // A lock lasts one cycle; it cannot chain.
      lock_q      <= |(gnt_o & lock_i) && !lock_q;
      lock_port_q <= idx_of(gnt_o);
      if (use_prom && |gnt_o)                                rr_prom_q <= idx_of(gnt_o);
      if (!use_prom && !use_lock && |(gnt_o & tx_mask))      rr_tx_q   <= idx_of(gnt_o);
      if (!use_prom && !use_lock && |(gnt_o & rx_mask))      rr_rx_q   <= idx_of(gnt_o);
      for (int i = 0; i < NPORTS; i++) begin
        if (req_i[i] && !gnt_o[i]) begin
          if (age_q[i] != AGEW'(STARVE_LIMIT)) age_q[i] <= age_q[i] + 1'b1;
        end else begin
          age_q[i] <= '0;
        end
      end
    end
  end

`ifndef SYNTHESIS
  // Starvation bound (SPEC.md): once promoted, a port waits at most one
  // round-robin turn over the other ports, plus one locked cycle per atomic.
  localparam int unsigned BOUND = STARVE_LIMIT + 2 * (NPORTS - 1) + 1;
  int unsigned wait_q [NPORTS];
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      for (int i = 0; i < NPORTS; i++) wait_q[i] <= 0;
    end else begin
      for (int i = 0; i < NPORTS; i++) begin
        wait_q[i] <= (req_i[i] && !gnt_o[i]) ? wait_q[i] + 1 : 0;
        assert (wait_q[i] <= BOUND)
          else $error("spm_arbiter: port %0d waited %0d cycles (bound %0d)", i, wait_q[i], BOUND);
      end
    end
  end
`endif

endmodule
