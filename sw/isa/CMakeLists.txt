# riscv-tests rv32ui and rv32um, built against the Torus tile environment.
set(RT ${CMAKE_SOURCE_DIR}/third_party/riscv-tests)
set(ISA_TESTS_ui add addi and andi auipc beq bge bgeu blt bltu bne simple fence_i jal jalr
    lb lbu lh lhu lw ld_st lui ma_data or ori sb sh sw st_ld sll slli slt slti sltiu sltu
    sra srai srl srli sub xor xori)
set(ISA_TESTS_um div divu mul mulh mulhsu mulhu rem remu)

set(ISA_ELFS "")
foreach(suite ui um)
  foreach(t IN LISTS ISA_TESTS_${suite})
    set(name rv32${suite}-${t})
    set(out ${CMAKE_BINARY_DIR}/sw/isa/${name}.elf)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sw/isa
      COMMAND ${RISCV_GCC} -march=rv32im_zicsr_zifencei -mabi=ilp32 -static -mcmodel=medany
              -fvisibility=hidden -nostdlib -nostartfiles
              -I${CMAKE_CURRENT_SOURCE_DIR}/env -I${CMAKE_SOURCE_DIR}/sw/sdk -I${RT}/env -I${RT}/isa/macros/scalar
              -T${CMAKE_CURRENT_SOURCE_DIR}/isa_link.ld -Wl,--no-warn-rwx-segments
              ${RT}/isa/rv32${suite}/${t}.S -o ${out}
      COMMAND ${RISCV_OBJDUMP} -d -M no-aliases ${out} > ${out}.dis
      DEPENDS ${RT}/isa/rv32${suite}/${t}.S ${CMAKE_CURRENT_SOURCE_DIR}/env/riscv_test.h ${CMAKE_SOURCE_DIR}/sw/sdk/torus_exit.h
              ${CMAKE_CURRENT_SOURCE_DIR}/isa_link.ld
      VERBATIM)
    list(APPEND ISA_ELFS ${out})
  endforeach()
endforeach()
add_custom_target(isa_tests ALL DEPENDS ${ISA_ELFS})
