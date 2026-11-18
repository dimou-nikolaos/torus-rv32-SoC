# The SoC model per grid size (a library: the simulator host, Spike lockstep,
# libtorus), and the programs that drive it: tb_soc runs ELFs on every tile,
# host_memcpy is the M2 exit test, host_cannon the M3 one.
# 8x8 (M4) runs multithreaded: TORUS_SOC_THREADS Verilator threads.
set(TORUS_SOC_THREADS 8 CACHE STRING "Verilator threads for the 8x8 SoC model")
foreach(grid "1;1;1" "2;2;1" "4;4;1" "8;8;${TORUS_SOC_THREADS}")
  list(GET grid 0 nx)
  list(GET grid 1 ny)
  list(GET grid 2 threads)
  set(lib soc_${nx}x${ny})
  torus_verilated_lib(${lib}
    TOP    tb_soc_top
    THREADS ${threads}
    FLISTS rtl/soc/soc_sim.f
    SV     tb/soc/tb_soc_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/sim_host.cpp
    ARGS   -GNX=${nx} -GNY=${ny})
  target_compile_definitions(${lib} PRIVATE SOC_NX=${nx} SOC_NY=${ny})
  target_include_directories(${lib} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${lib} PUBLIC spike_cosim torus)

  add_executable(tb_soc_${nx}x${ny} ${CMAKE_CURRENT_SOURCE_DIR}/tb_soc.cpp)
  target_link_libraries(tb_soc_${nx}x${ny} PRIVATE ${lib})
  set_target_properties(tb_soc_${nx}x${ny} PROPERTIES BUILD_RPATH ${SPIKE_PREFIX}/lib)
  foreach(host host_memcpy host_cannon host_stencil host_reduce)
    add_executable(${host}_${nx}x${ny} ${CMAKE_CURRENT_SOURCE_DIR}/${host}.cpp)
    target_link_libraries(${host}_${nx}x${ny} PRIVATE ${lib})
    set_target_properties(${host}_${nx}x${ny} PROPERTIES BUILD_RPATH ${SPIKE_PREFIX}/lib)
  endforeach()
endforeach()

# The 4x4 SoC with its links routed through the tiles' feedthroughs, as placed
# in logical (ft1) and folded (ft2) order (M5): only wiring changes, so the
# same tests must pass with the same results.
foreach(ft 1 2)
  set(lib soc_4x4_ft${ft})
  torus_verilated_lib(${lib}
    TOP    tb_soc_top
    FLISTS rtl/soc/soc_sim.f
    SV     tb/soc/tb_soc_top.sv
    CPP    ${CMAKE_CURRENT_SOURCE_DIR}/sim_host.cpp
    ARGS   -GNX=4 -GNY=4 -GFT_ORDER=${ft})
  target_compile_definitions(${lib} PRIVATE SOC_NX=4 SOC_NY=4)
  target_include_directories(${lib} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${lib} PUBLIC spike_cosim torus)
  foreach(host host_memcpy host_cannon)
    add_executable(${host}_4x4_ft${ft} ${CMAKE_CURRENT_SOURCE_DIR}/${host}.cpp)
    target_link_libraries(${host}_4x4_ft${ft} PRIVATE ${lib})
    set_target_properties(${host}_4x4_ft${ft} PROPERTIES BUILD_RPATH ${SPIKE_PREFIX}/lib)
  endforeach()
endforeach()
