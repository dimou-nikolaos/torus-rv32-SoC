torus_verilated_tb(tb_spm
  TOP   tb_spm_top
  FLISTS rtl/tile/spm.f
  SV    tb/unit/spm/tb_spm_top.sv
  CPP   ${CMAKE_CURRENT_SOURCE_DIR}/spm/tb_spm.cpp)

# Credit link: full-depth buffers at slice depths 0..2, plus one shallow buffer
# (DEPTH = RTT - 1) that must lose exactly the predicted throughput.
foreach(cfg "0;4;0" "1;6;0" "2;8;0" "0;3;1" "2;7;1")
  list(GET cfg 0 slice)
  list(GET cfg 1 depth)
  list(GET cfg 2 shallow)
  set(name tb_link_s${slice}_d${depth})
  torus_verilated_tb(${name}
    TOP    tb_link_top
    FLISTS rtl/noc/noc.f
    SV     tb/unit/link/tb_link_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/link/tb_link.cpp
    ARGS   -GSLICE=${slice} -GDEPTH=${depth} -GALLOW_SHALLOW=${shallow})
  target_compile_definitions(${name} PRIVATE LINK_SLICE=${slice} LINK_DEPTH=${depth})
endforeach()

torus_verilated_tb(tb_flit
  TOP    tb_flit_top
  FLISTS rtl/noc/noc.f
  SV     tb/unit/flit/tb_flit_top.sv
  CPP    ${CMAKE_CURRENT_SOURCE_DIR}/flit/tb_flit.cpp)
