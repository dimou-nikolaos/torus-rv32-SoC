# SoC testbench: 1x1 (single tile, the M0 tests) and 2x2 (M1)
foreach(grid "1;1" "2;2")
  list(GET grid 0 nx)
  list(GET grid 1 ny)
  set(name tb_soc_${nx}x${ny})
  torus_verilated_tb(${name}
    TOP    tb_soc_top
    FLISTS rtl/soc/soc_sim.f
    SV     tb/soc/tb_soc_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/tb_soc.cpp
    ARGS   -GNX=${nx} -GNY=${ny})
  target_compile_definitions(${name} PRIVATE SOC_NX=${nx} SOC_NY=${ny})
  target_link_libraries(${name} PRIVATE spike_cosim)
  set_target_properties(${name} PROPERTIES BUILD_RPATH ${SPIKE_PREFIX}/lib)
endforeach()
