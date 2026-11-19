# Scratchpad banks at both sizes (TORUS_SPM_KB), behavioral and built from sky130
# SRAM macros (place and route), the latter run on OpenRAM's behavioral model of
# the macro from the image's PDK: one macro row per bank at 32 KB, two at 64 KB.
set(SKY130_SRAM_V
    $ENV{PDK_ROOT}/sky130A/libs.ref/sky130_sram_macros/verilog/sky130_sram_2kbyte_1rw1r_32x512_8.v)
if(NOT EXISTS ${SKY130_SRAM_V})
  message(FATAL_ERROR "sky130 SRAM macro model not found: ${SKY130_SRAM_V} (PDK_ROOT from the tool image)")
endif()
foreach(kb 32 64)
  torus_verilated_tb(tb_spm_${kb}
    TOP   tb_spm_top
    SPM_KB ${kb}
    FLISTS rtl/tile/spm.f
    SV    tb/unit/spm/tb_spm_top.sv
    CPP   ${CMAKE_CURRENT_SOURCE_DIR}/spm/tb_spm.cpp)
  torus_verilated_tb(tb_spm_sky130_${kb}
    TOP   tb_spm_top
    SPM_KB ${kb}
    FLISTS rtl/tile/spm.f
    SV    tb/unit/spm/tb_spm_top.sv rtl/tile/spm_bank_sky130.sv
    CPP   ${CMAKE_CURRENT_SOURCE_DIR}/spm/tb_spm.cpp
    ARGS  +define+SPM_SKY130 ${SKY130_SRAM_V})
endforeach()

# The same on IHP SG13G2 SRAM macros (two per bank: 512x64 at 32 KB, 1024x64 at
# 64 KB), on the PDK's functional model.
set(IHP_SRAM_DIR $ENV{PDK_ROOT}/ihp-sg13g2/libs.ref/sg13g2_sram/verilog)
if(NOT EXISTS ${IHP_SRAM_DIR}/RM_IHPSG13_1P_core_behavioral_bm_bist.v)
  message(FATAL_ERROR "IHP SRAM macro models not found in ${IHP_SRAM_DIR} (PDK_ROOT from the tool image)")
endif()
foreach(cfg "32;512" "64;1024")
  list(GET cfg 0 kb)
  list(GET cfg 1 rows)
  torus_verilated_tb(tb_spm_ihp_${kb}
    TOP   tb_spm_top
    SPM_KB ${kb}
    FLISTS rtl/tile/spm.f
    SV    tb/unit/spm/tb_spm_top.sv rtl/tile/spm_bank_ihp.sv
    CPP   ${CMAKE_CURRENT_SOURCE_DIR}/spm/tb_spm.cpp
    ARGS  --timescale 1ns/10ps +define+SPM_IHP +define+FUNCTIONAL ${IHP_SRAM_DIR}/RM_IHPSG13_1P_${rows}x64_c2_bm_bist.v
          ${IHP_SRAM_DIR}/RM_IHPSG13_1P_core_behavioral_bm_bist.v)
endforeach()

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

# Bare NoC: single planes (2x2 and 4x4, both directions, slice depths, odd
# rings as with the I/O row), both planes side by side with plane selection and
# the I/O row's router-only stops, and builds without the bubble rule that must
# deadlock. Those start at 3x3: on rings of length 2 a flit never takes two
# hops in one dimension, so no ring can form a cyclic wait.
# Fields: gx gy dir slice bubble planes io_row
foreach(cfg "2;2;0;0;1;1;0" "2;2;1;0;1;1;0" "4;4;0;0;1;1;0" "4;4;1;1;1;1;0" "4;4;0;2;1;1;0" "3;5;0;0;1;1;0"
            "3;5;1;0;1;1;0" "3;3;0;0;0;1;0" "4;4;0;0;0;1;0"
            "4;4;0;0;1;2;0" "2;3;0;0;1;2;1" "4;5;0;0;1;1;1" "4;5;0;0;1;2;1" "4;5;0;1;1;2;1" "3;4;0;2;1;2;1")
  list(GET cfg 0 gx)
  list(GET cfg 1 gy)
  list(GET cfg 2 dir)
  list(GET cfg 3 slice)
  list(GET cfg 4 bubble)
  list(GET cfg 5 planes)
  list(GET cfg 6 io)
  if(planes EQUAL 2)
    set(name tb_noc_${gx}x${gy}_p01_s${slice})
  else()
    set(name tb_noc_${gx}x${gy}_p${dir}_s${slice})
  endif()
  if(io)
    set(name ${name}_io)
  endif()
  if(NOT bubble)
    set(name ${name}_nobubble)
  endif()
  torus_verilated_tb(${name}
    TOP    tb_noc_top
    FLISTS rtl/noc/noc.f
    SV     tb/unit/noc/tb_noc_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/noc/tb_noc.cpp
    ARGS   -GGX=${gx} -GGY=${gy} -GDIR=${dir} -GSLICE=${slice} -GBUBBLE=${bubble} -GPLANES=${planes} -GIO_ROW=${io})
  target_compile_definitions(${name} PRIVATE N_GX=${gx} N_GY=${gy} N_DIR=${dir} N_SLICE=${slice}
                             N_PLANES=${planes} N_IO_ROW=${io})
endforeach()

foreach(kb 32 64)
  torus_verilated_tb(tb_niu_${kb}
    TOP    tb_niu_top
    SPM_KB ${kb}
    FLISTS rtl/tile/niu.f
    SV     tb/unit/niu/tb_niu_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/niu/tb_niu.cpp)
endforeach()

# Async FIFO (the I/O tile's clock crossing): two depths
foreach(depth 8 16)
  set(name tb_cdc_d${depth})
  torus_verilated_tb(${name}
    TOP    tb_cdc_top
    SV     rtl/special/io_cdc_fifo.sv tb/unit/cdc/tb_cdc_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/cdc/tb_cdc.cpp
    ARGS   -GDEPTH=${depth})
endforeach()

# I/O tile: AXI front-end, crossings, bridge, register block
torus_verilated_tb(tb_io
  TOP    tb_io_top
  FLISTS rtl/noc/noc.f rtl/special/io.f
  SV     tb/unit/io/tb_io_top.sv
  CPP    ${CMAKE_CURRENT_SOURCE_DIR}/io/tb_io.cpp)
