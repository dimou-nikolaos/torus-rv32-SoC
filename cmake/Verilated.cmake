# Helpers for Verilator testbenches.
find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})

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

# torus_verilated_tb(<name> TOP <module> FLISTS <f>... SV <extra .sv>... CPP <sources>...)
# Builds executable <name> and registers it with CTest (runs with --seed from env).
function(torus_verilated_tb name)
  cmake_parse_arguments(TB "" "TOP" "FLISTS;SV;CPP;ARGS" ${ARGN})
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
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tb/common ${CMAKE_SOURCE_DIR}/tb/models)
  verilate(${name}
    SOURCES ${srcs}
    TOP_MODULE ${TB_TOP}
    PREFIX V${TB_TOP}
    TRACE_FST
    VERILATOR_ARGS -Wall --assert --x-assign unique --x-initial unique --x-initial-edge -O3
                   --timing ${opts} ${TB_ARGS})
endfunction()
