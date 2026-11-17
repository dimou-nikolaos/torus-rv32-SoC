torus_verilated_tb(tb_spm
  TOP   tb_spm_top
  FLISTS rtl/tile/spm.f
  SV    tb/unit/spm/tb_spm_top.sv
  CPP   ${CMAKE_CURRENT_SOURCE_DIR}/spm/tb_spm.cpp)
