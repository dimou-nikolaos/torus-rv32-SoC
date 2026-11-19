// Compute tile for place and route in IHP SG13G2 (scripts/pnr.py --platform ihp;
// SYNTHESIS defined by the flow, SPM_IHP by scripts/pnr.py): SRAM macro banks
// and the library clock gate (paths relative to repo root)
-f rtl/core/cv32e40x_rtl.f
rtl/core/cv32e40x_clock_gate_ihp.sv
rtl/core/core_wrap.sv
rtl/pkg/tile_pkg.sv
-f rtl/noc/noc.f
rtl/tile/niu.sv
rtl/tile/reg_rsp_stage.sv
rtl/tile/spm_bank_ihp.sv
rtl/tile/spm_bank.sv
rtl/tile/spm_arbiter.sv
rtl/tile/spm_banks.sv
rtl/tile/obi_spm_adapter.sv
rtl/tile/tile_ctrl.sv
rtl/tile/sim_console.sv
rtl/perf/counter_bank.sv
rtl/tile/tile_top.sv
