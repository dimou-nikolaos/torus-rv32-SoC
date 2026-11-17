cmake_minimum_required(VERSION 3.20)
project(torus_rv32_soc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(ExternalProject)
include(ProcessorCount)
ProcessorCount(NPROC)

# Spike, built from the pinned submodule (patches live in the submodule history).
# An install whose stamp matches the submodule commit is reused (CI caches it).
set(SPIKE_PREFIX ${CMAKE_BINARY_DIR}/spike-install CACHE PATH "Spike install prefix")
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR}/third_party/spike rev-parse HEAD
                OUTPUT_VARIABLE SPIKE_SHA OUTPUT_STRIP_TRAILING_WHITESPACE)
set(SPIKE_STAMP ${SPIKE_PREFIX}/.torus-spike-sha)
set(spike_cached FALSE)
if(EXISTS ${SPIKE_STAMP})
  file(READ ${SPIKE_STAMP} cached_sha)
  string(STRIP "${cached_sha}" cached_sha)
  if(cached_sha STREQUAL SPIKE_SHA AND EXISTS ${SPIKE_PREFIX}/lib/libriscv.so)
    set(spike_cached TRUE)
  endif()
endif()
if(spike_cached)
  message(STATUS "Spike: using cached install ${SPIKE_PREFIX} (${SPIKE_SHA})")
  add_custom_target(spike)
else()
  ExternalProject_Add(spike
    SOURCE_DIR        ${CMAKE_SOURCE_DIR}/third_party/spike
    BINARY_DIR        ${CMAKE_BINARY_DIR}/spike-build
    INSTALL_DIR       ${SPIKE_PREFIX}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND     make -j${NPROC}
    INSTALL_COMMAND   make install COMMAND ${CMAKE_COMMAND} -E echo_append ${SPIKE_SHA} > ${SPIKE_STAMP}
    BUILD_BYPRODUCTS  ${SPIKE_PREFIX}/bin/spike ${SPIKE_PREFIX}/lib/libriscv.so
    USES_TERMINAL_BUILD ON
  )
endif()
set(SPIKE_BIN ${SPIKE_PREFIX}/bin/spike CACHE INTERNAL "")

# Verilator testbenches
include(cmake/Verilated.cmake)
enable_testing()
add_subdirectory(tb)

# Tile software
include(cmake/Kernels.cmake)
add_subdirectory(sw)
