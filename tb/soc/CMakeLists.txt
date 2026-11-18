# The SoC model per grid size (a library: the simulator host, Spike lockstep,
# libtorus), and the programs that drive it: tb_soc runs ELFs on every tile,
# host_memcpy is the M2 exit test, host_cannon the M3 one.
foreach(grid "1;1" "2;2" "4;4")
  list(GET grid 0 nx)
  list(GET grid 1 ny)
  set(lib soc_${nx}x${ny})
  torus_verilated_lib(${lib}
    TOP    tb_soc_top
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
  foreach(host host_memcpy host_cannon)
    add_executable(${host}_${nx}x${ny} ${CMAKE_CURRENT_SOURCE_DIR}/${host}.cpp)
    target_link_libraries(${host}_${nx}x${ny} PRIVATE ${lib})
    set_target_properties(${host}_${nx}x${ny} PROPERTIES BUILD_RPATH ${SPIKE_PREFIX}/lib)
  endforeach()
endforeach()
