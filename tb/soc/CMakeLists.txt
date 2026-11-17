torus_verilated_tb(tb_tile
  TOP    tb_tile_top
  FLISTS rtl/tile/tile_sim.f
  SV     tb/soc/tb_tile_top.sv
  CPP    ${CMAKE_CURRENT_SOURCE_DIR}/tb_tile.cpp)
target_link_libraries(tb_tile PRIVATE spike_cosim)
set_target_properties(tb_tile PROPERTIES BUILD_RPATH ${SPIKE_PREFIX}/lib)
