# Helpers for Verilator testbenches.
#
# Verilator's *__Slow.cpp files hold construction and initialization, run once:
# they are compiled at -O0 (a 4x4 SoC's largest one takes 8 s instead of 13
# minutes at -O3); the per-cycle code keeps the build type's optimization.
find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})

# Scratchpad size per tile in KB (tile_pkg.sv TORUS_SPM_KB): 32 or 64. A model
# can override it with SPM_KB (the 64 KB regression configuration); its C++
# sees the same value (tb/models/spm_model.h).
set(TORUS_SPM_KB 32 CACHE STRING "Scratchpad size per tile in KB (32 or 64)")

# FST waveforms (--trace=FILE) cost build and run time in every model, so they
# are compiled in only on request: cmake -DTORUS_TRACE=ON (tb/common/tb_trace.h).
option(TORUS_TRACE "Compile FST waveform tracing into the testbenches" OFF)
set(TORUS_TRACE_OPT "")
if(TORUS_TRACE)
  set(TORUS_TRACE_OPT TRACE_FST)
endif()

# Expand a .f file list (paths relative to the repo root, nested -f allowed)
# into a flat list of absolute paths and +incdir/+define options.
function(torus_expand_flist flist out_var)
  file(STRINGS ${CMAKE_SOURCE_DIR}/${flist} lines)
  set(result "")
  foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^//")
      continue()
    elseif(line MATCHES "^-f[ \t]+(.+)$")
      torus_expand_flist(${CMAKE_MATCH_1} nested)
      list(APPEND result ${nested})
    elseif(line MATCHES "^\\+incdir\\+(.+)$")
      list(APPEND result "-I${CMAKE_SOURCE_DIR}/${CMAKE_MATCH_1}")
    elseif(line MATCHES "^\\+define\\+(.+)$")
      list(APPEND result "-D${CMAKE_MATCH_1}")
    else()
      list(APPEND result ${CMAKE_SOURCE_DIR}/${line})
    endif()
  endforeach()
  set(${out_var} ${result} PARENT_SCOPE)
endfunction()

# torus_verilated_tb(<name> TOP <module> [SPM_KB <kb>] FLISTS <f>... SV <extra .sv>... CPP <sources>...)
# Builds executable <name> and registers it with CTest (runs with --seed from env).
function(torus_verilated_tb name)
  cmake_parse_arguments(TB "" "TOP;SPM_KB" "FLISTS;SV;CPP;ARGS" ${ARGN})
  if(NOT TB_SPM_KB)
    set(TB_SPM_KB ${TORUS_SPM_KB})
  endif()
  set(srcs "")
  set(opts "")
  foreach(f IN LISTS TB_FLISTS)
    torus_expand_flist(${f} items)
    foreach(i IN LISTS items)
      if(i MATCHES "^-")
        list(APPEND opts ${i})
      else()
        list(APPEND srcs ${i})
      endif()
    endforeach()
  endforeach()
  foreach(f IN LISTS TB_SV)
    list(APPEND srcs ${CMAKE_SOURCE_DIR}/${f})
  endforeach()
  add_executable(${name} ${TB_CPP})
  target_compile_definitions(${name} PRIVATE TORUS_SPM_KB=${TB_SPM_KB})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tb/common ${CMAKE_SOURCE_DIR}/tb/models
                             ${CMAKE_SOURCE_DIR}/sw/runtime)
  verilate(${name}
    SOURCES ${srcs}
    TOP_MODULE ${TB_TOP}
    PREFIX V${TB_TOP}
    ${TORUS_TRACE_OPT}
    OPT_SLOW -O0
    VERILATOR_ARGS -Wall -Werror-USERFATAL -Werror-USERERROR --assert
                   --x-assign unique --x-initial unique --x-initial-edge -O3
                   --timing ${CMAKE_SOURCE_DIR}/rtl/lint/vendor.vlt ${opts} -DTORUS_SPM_KB=${TB_SPM_KB} ${TB_ARGS})
endfunction()

# torus_verilated_lib(<name> TOP <module> [THREADS <n>] [SPM_KB <kb>] FLISTS <f>... SV <extra .sv>... CPP <sources>...)
# The same as a static library, for several programs sharing one model;
# THREADS > 1 builds a multithreaded model.
function(torus_verilated_lib name)
  cmake_parse_arguments(TB "" "TOP;THREADS;SPM_KB" "FLISTS;SV;CPP;ARGS" ${ARGN})
  if(NOT TB_SPM_KB)
    set(TB_SPM_KB ${TORUS_SPM_KB})
  endif()
  if(NOT TB_THREADS)
    set(TB_THREADS 1)
  endif()
  set(srcs "")
  set(opts "")
  foreach(f IN LISTS TB_FLISTS)
    torus_expand_flist(${f} items)
    foreach(i IN LISTS items)
      if(i MATCHES "^-")
        list(APPEND opts ${i})
      else()
        list(APPEND srcs ${i})
      endif()
    endforeach()
  endforeach()
  foreach(f IN LISTS TB_SV)
    list(APPEND srcs ${CMAKE_SOURCE_DIR}/${f})
  endforeach()
  add_library(${name} STATIC ${TB_CPP})
  target_compile_definitions(${name} PUBLIC TORUS_SPM_KB=${TB_SPM_KB})
  target_include_directories(${name} PUBLIC ${CMAKE_SOURCE_DIR}/tb/common ${CMAKE_SOURCE_DIR}/tb/models)
  verilate(${name}
    SOURCES ${srcs}
    TOP_MODULE ${TB_TOP}
    PREFIX V${TB_TOP}
    THREADS ${TB_THREADS}
    ${TORUS_TRACE_OPT}
    OPT_SLOW -O0
    VERILATOR_ARGS -Wall -Werror-USERFATAL -Werror-USERERROR --assert
                   --x-assign unique --x-initial unique --x-initial-edge -O3
                   --timing ${CMAKE_SOURCE_DIR}/rtl/lint/vendor.vlt ${opts} -DTORUS_SPM_KB=${TB_SPM_KB} ${TB_ARGS})
endfunction()
