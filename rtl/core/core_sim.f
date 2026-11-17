// Core for simulation: vendor RTL + vendor sim wrapper with RVFI (assertions off)
+define+COREV_ASSERT_OFF
-f rtl/core/cv32e40x_rtl.f
+incdir+rtl/core/cv32e40x/bhv
+incdir+rtl/core/cv32e40x/bhv/include
+incdir+rtl/core/cv32e40x/sva
rtl/core/cv32e40x/bhv/include/cv32e40x_rvfi_pkg.sv
rtl/core/cv32e40x/bhv/cv32e40x_sim_clock_gate.sv
rtl/core/cv32e40x/bhv/cv32e40x_rvfi_instr_obi.sv
rtl/core/cv32e40x/bhv/cv32e40x_rvfi_data_obi.sv
rtl/core/cv32e40x/bhv/cv32e40x_rvfi.sv
rtl/core/cv32e40x/bhv/cv32e40x_wrapper.sv
rtl/core/core_wrap.sv
