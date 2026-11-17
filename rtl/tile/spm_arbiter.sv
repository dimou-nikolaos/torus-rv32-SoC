// Bank arbiter: priority with a starvation bound (SPEC.md, Scratchpad bank arbiter).
//
//   1. Lock: a port granted with `lock` owns the bank for the next cycle if it
//      requests again (the write half of an atomic read-modify-write).
//   2. Promoted ports (denied STARVE_LIMIT consecutive cycles) win over all
//      default priorities; several promoted ports are served round-robin.
//   3. Default order: engine, NIU ports (round-robin), core data, core instruction.
//
// Pure arbitration: one-hot grant per cycle, no data path.
module spm_arbiter #(
  parameter int unsigned NPORTS       = tile_pkg::SPM_PORTS,
  parameter int unsigned STARVE_LIMIT = 4,
  parameter int unsigned RR_LO        = tile_pkg::NIU_PORT_LO,  // round-robin group
  parameter int unsigned RR_HI        = tile_pkg::NIU_PORT_HI,
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
    assert (RR_LO <= RR_HI && RR_HI < NPORTS) else $fatal(1, "bad round-robin group");
  end

  logic [AGEW-1:0]   age_q [NPORTS];
  logic              lock_q;
  logic [PW-1:0]     lock_port_q;
  logic [PW-1:0]     rr_prom_q, rr_grp_q;   // last winner of each round-robin
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

  logic [NPORTS-1:0] grp_mask;
  always_comb
    for (int i = 0; i < NPORTS; i++)
      grp_mask[i] = (i >= RR_LO) && (i <= RR_HI);

  logic use_prom;
  always_comb begin
    gnt_o    = '0;
    use_prom = 1'b0;
    if (lock_q && req_i[lock_port_q]) begin
      gnt_o[lock_port_q] = 1'b1;
    end else if (|promoted) begin
      use_prom = 1'b1;
      gnt_o    = rr_pick(promoted, 0, NPORTS - 1, rr_prom_q);
    end else begin
      for (int i = NPORTS - 1; i >= 0; i--) begin
        // Fixed priority by index outside the group; the group competes as a
        // whole at the priority of its lowest index.
        if (req_i[i] && !grp_mask[i]) begin
          gnt_o    = '0;
          gnt_o[i] = 1'b1;
        end
        if (i == RR_LO && |(req_i & grp_mask)) begin
          gnt_o = rr_pick(req_i & grp_mask, RR_LO, RR_HI, rr_grp_q);
        end
      end
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
      rr_grp_q    <= PW'(RR_HI);
      for (int i = 0; i < NPORTS; i++) age_q[i] <= '0;
    end else begin
      // A lock lasts one cycle; it cannot chain.
      lock_q      <= |(gnt_o & lock_i) && !lock_q;
      lock_port_q <= idx_of(gnt_o);
      if (use_prom && |gnt_o)                    rr_prom_q <= idx_of(gnt_o);
      if (!use_prom && |(gnt_o & grp_mask) && !(lock_q && req_i[lock_port_q]))
                                                 rr_grp_q  <= idx_of(gnt_o);
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
