// Scratchpad: SPM_BANKS banks of 128 bit, interleaved on address bits [5:4], each
// with its own arbiter over SPM_PORTS master ports. Every granted request (read or
// write) gets rvalid exactly one cycle later; read data comes with it.
module spm_banks
  import tile_pkg::*;
#(
  parameter int unsigned STARVE_LIMIT = 4
) (
  input  logic                 clk_i,
  input  logic                 rst_ni,
  input  spm_req_t             req_i [SPM_PORTS],
  output spm_rsp_t             rsp_o [SPM_PORTS],
  // Per-port events for the counter bank (a port targets one bank per cycle)
  output logic [SPM_PORTS-1:0] denied_o,
  output logic [SPM_PORTS-1:0] promoted_o
);
  localparam int unsigned BW = $clog2(SPM_BANKS);

  logic [SPM_PORTS-1:0] bank_req  [SPM_BANKS];
  logic [SPM_PORTS-1:0] bank_lock [SPM_BANKS];
  logic [SPM_PORTS-1:0] bank_gnt  [SPM_BANKS];
  logic [SPM_PORTS-1:0] bank_den  [SPM_BANKS];
  logic [SPM_PORTS-1:0] bank_prom [SPM_BANKS];
  logic [127:0]         bank_rdata[SPM_BANKS];

  always_comb begin
    for (int b = 0; b < SPM_BANKS; b++) begin
      for (int p = 0; p < SPM_PORTS; p++) begin
        bank_req[b][p]  = req_i[p].req && (spm_bank_of(req_i[p].addr) == BW'(b));
        bank_lock[b][p] = req_i[p].lock;
      end
    end
  end

  for (genvar b = 0; b < SPM_BANKS; b++) begin : g_bank
    logic                  en, we;
    logic [SPM_ROW_AW-1:0] row;
    logic [15:0]           be;
    logic [127:0]          wdata;

    spm_arbiter #(.NPORTS(SPM_PORTS), .STARVE_LIMIT(STARVE_LIMIT)) u_arb (
      .clk_i, .rst_ni,
      .req_i      (bank_req[b]),
      .lock_i     (bank_lock[b]),
      .gnt_o      (bank_gnt[b]),
      .denied_o   (bank_den[b]),
      .promoted_o (bank_prom[b])
    );

    always_comb begin
      en = |bank_gnt[b];
      we = 1'b0; row = '0; be = '0; wdata = '0;
      for (int p = 0; p < SPM_PORTS; p++) begin
        if (bank_gnt[b][p]) begin
          we    = req_i[p].we;
          row   = spm_row_of(req_i[p].addr);
          be    = req_i[p].be;
          wdata = req_i[p].wdata;
        end
      end
    end

    spm_bank #(.ROWS(SPM_ROWS)) u_bank (
      .clk_i, .en_i(en), .we_i(we), .row_i(row), .be_i(be), .wdata_i(wdata),
      .rdata_o(bank_rdata[b])
    );
  end

  // Response routing
  logic [SPM_PORTS-1:0] gnt, rvalid_q;
  logic [BW-1:0]        bank_q [SPM_PORTS];

  always_comb begin
    gnt        = '0;
    denied_o   = '0;
    promoted_o = '0;
    for (int b = 0; b < SPM_BANKS; b++) begin
      gnt        |= bank_gnt[b];
      denied_o   |= bank_den[b];
      promoted_o |= bank_prom[b];
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rvalid_q <= '0;
      for (int p = 0; p < SPM_PORTS; p++) bank_q[p] <= '0;
    end else begin
      rvalid_q <= gnt;
      for (int p = 0; p < SPM_PORTS; p++)
        if (gnt[p]) bank_q[p] <= spm_bank_of(req_i[p].addr);
    end
  end

  always_comb
    for (int p = 0; p < SPM_PORTS; p++) begin
      rsp_o[p].gnt    = gnt[p];
      rsp_o[p].rvalid = rvalid_q[p];
      rsp_o[p].rdata  = bank_rdata[bank_q[p]];
    end

endmodule
