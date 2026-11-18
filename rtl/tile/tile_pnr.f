// Compute tile for place and route in sky130 (scripts/pnr.py; SYNTHESIS defined
// by the flow, SPM_SKY130 by pnr/tile/config.mk): SRAM macro banks and the
// library clock gate (paths relative to repo root)
-f rtl/core/cv32e40x_rtl.f
rtl/core/cv32e40x_clock_gate_sky130.sv
rtl/core/core_wrap.sv
rtl/pkg/tile_pkg.sv
-f rtl/noc/noc.f
rtl/tile/niu.sv
rtl/tile/reg_rsp_stage.sv
rtl/tile/spm_bank_sky130.sv
rtl/tile/spm_bank.sv
rtl/tile/spm_arbiter.sv
rtl/tile/spm_banks.sv
rtl/tile/obi_spm_adapter.sv
rtl/tile/tile_ctrl.sv
rtl/tile/sim_console.sv
rtl/perf/counter_bank.sv
rtl/tile/tile_top.sv
