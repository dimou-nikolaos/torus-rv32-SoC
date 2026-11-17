# Cross-compiled tile programs. Uses the pinned riscv-none-elf toolchain directly
# (custom commands), so one CMake tree builds both host and tile code.
find_program(RISCV_GCC riscv-none-elf-gcc REQUIRED)
find_program(RISCV_OBJDUMP riscv-none-elf-objdump REQUIRED)

set(TILE_MARCH "rv32im_zicsr" CACHE STRING "Tile -march")
set(TILE_CFLAGS -march=${TILE_MARCH} -mabi=ilp32 -O2 -g -Wall -Wextra -Werror
    -ffunction-sections -fdata-sections -mcmodel=medlow)
set(TILE_LDFLAGS -march=${TILE_MARCH} -mabi=ilp32 --specs=nano.specs -nostartfiles
    -Wl,--gc-sections -Wl,--no-warn-rwx-segments -T ${CMAKE_SOURCE_DIR}/sw/sdk/link.ld)
set(TILE_SDK_SRCS ${CMAKE_SOURCE_DIR}/sw/sdk/crt0.S ${CMAKE_SOURCE_DIR}/sw/sdk/tile.c)

# torus_tile_program(<name> SOURCES <files>... [INCLUDES <dirs>...] [NO_SDK] [LDSCRIPT <ld>])
# Produces ${CMAKE_BINARY_DIR}/sw/<name>.elf plus .map and .dis.
function(torus_tile_program name)
  cmake_parse_arguments(K "NO_SDK" "LDSCRIPT" "SOURCES;INCLUDES;DEFINES" ${ARGN})
  set(out ${CMAKE_BINARY_DIR}/sw/${name}.elf)
  set(srcs ${K_SOURCES})
  set(ldflags ${TILE_LDFLAGS})
  if(NOT K_NO_SDK)
    list(APPEND srcs ${TILE_SDK_SRCS})
  endif()
  if(K_LDSCRIPT)
    list(REMOVE_ITEM ldflags -T ${CMAKE_SOURCE_DIR}/sw/sdk/link.ld)
    list(APPEND ldflags -T ${K_LDSCRIPT})
  endif()
  set(incs -I${CMAKE_SOURCE_DIR}/sw/sdk)
  foreach(i IN LISTS K_INCLUDES)
    list(APPEND incs -I${i})
  endforeach()
  set(defs "")
  foreach(d IN LISTS K_DEFINES)
    list(APPEND defs -D${d})
  endforeach()
  add_custom_command(
    OUTPUT ${out} ${CMAKE_BINARY_DIR}/sw/${name}.map ${CMAKE_BINARY_DIR}/sw/${name}.dis
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sw
    COMMAND ${RISCV_GCC} ${TILE_CFLAGS} ${incs} ${defs} ${srcs} ${ldflags}
            -Wl,-Map=${CMAKE_BINARY_DIR}/sw/${name}.map -o ${out}
    COMMAND ${RISCV_OBJDUMP} -d -M no-aliases ${out} > ${CMAKE_BINARY_DIR}/sw/${name}.dis
    DEPENDS ${srcs} ${CMAKE_SOURCE_DIR}/sw/sdk/link.ld ${CMAKE_SOURCE_DIR}/sw/sdk/tile.h
            ${CMAKE_SOURCE_DIR}/sw/sdk/torus_exit.h ${K_LDSCRIPT}
    COMMENT "Tile program ${name}"
    VERBATIM)
  add_custom_target(${name} ALL DEPENDS ${out})
endfunction()
