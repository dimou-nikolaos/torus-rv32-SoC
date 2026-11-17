add_subdirectory(unit)

# Spike as a library for lockstep co-simulation
add_library(spike_cosim STATIC ${CMAKE_CURRENT_SOURCE_DIR}/models/spike_cosim.cpp)
add_dependencies(spike_cosim spike)
target_include_directories(spike_cosim PUBLIC ${SPIKE_PREFIX}/include ${CMAKE_CURRENT_SOURCE_DIR}/models)
target_link_libraries(spike_cosim PUBLIC
  ${SPIKE_PREFIX}/lib/libriscv.so ${SPIKE_PREFIX}/lib/libsoftfloat.so
  ${SPIKE_PREFIX}/lib/libdisasm.a ${SPIKE_PREFIX}/lib/libfesvr.a dl pthread)
set_target_properties(spike_cosim PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(soc)
