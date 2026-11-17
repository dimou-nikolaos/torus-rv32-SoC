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

# Router: both planes, slice depths 0..2
foreach(dir 0 1)
  foreach(slice 0 1 2)
    math(EXPR depth "5 + 2 * ${slice}")
    set(name tb_router_p${dir}_s${slice})
    torus_verilated_tb(${name}
      TOP    tb_router_top
      FLISTS rtl/noc/noc.f
      SV     tb/unit/router/tb_router_top.sv
      CPP    ${CMAKE_CURRENT_SOURCE_DIR}/router/tb_router.cpp
      ARGS   -GDIR=${dir} -GSLICE=${slice} -GDEPTH=${depth})
    target_compile_definitions(${name} PRIVATE R_DIR=${dir} R_SLICE=${slice})
  endforeach()
endforeach()

# Bare NoC plane: 2x2 and 4x4, both directions, slice depths, an odd ring
# (3x5, as with the I/O row), and builds without the bubble rule that must
# deadlock. Those start at 3x3: on rings of length 2 a flit never takes two
# hops in one dimension, so no ring can form a cyclic wait.
foreach(cfg "2;2;0;0;1" "2;2;1;0;1" "4;4;0;0;1" "4;4;1;1;1" "4;4;0;2;1" "3;5;0;0;1" "3;3;0;0;0" "4;4;0;0;0")
  list(GET cfg 0 gx)
  list(GET cfg 1 gy)
  list(GET cfg 2 dir)
  list(GET cfg 3 slice)
  list(GET cfg 4 bubble)
  set(name tb_noc_${gx}x${gy}_p${dir}_s${slice})
  if(NOT bubble)
    set(name ${name}_nobubble)
  endif()
  torus_verilated_tb(${name}
    TOP    tb_noc_top
    FLISTS rtl/noc/noc.f
    SV     tb/unit/noc/tb_noc_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/noc/tb_noc.cpp
    ARGS   -GGX=${gx} -GGY=${gy} -GDIR=${dir} -GSLICE=${slice} -GBUBBLE=${bubble})
  target_compile_definitions(${name} PRIVATE N_GX=${gx} N_GY=${gy} N_DIR=${dir} N_SLICE=${slice})
endforeach()
