# Tile start-up: gp, sp, trap vector, clear .bss, then
#   exit(kernel_main((const args_t *)__mailbox))
# A trap prints its cause and ends the simulation with exit code 0x80 | mcause.

  .section .text.init, "ax"
  .globl _start
_start:
  .option push
  .option norelax
  la    gp, __global_pointer$
  .option pop
  la    sp, __stack_top
  la    t0, trap_entry
  csrw  mtvec, t0

  la    t0, __bss_start
  la    t1, __bss_end
1:
  bgeu  t0, t1, 2f
  sw    zero, 0(t0)
  addi  t0, t0, 4
  j     1b
2:
  la    a0, __mailbox
  call  kernel_main
  call  exit
3:
  j     3b

  # CV32E40X keeps only mtvec[31:7]: the trap entry must be 128-byte aligned.
  .align 7
trap_entry:
  csrr  a0, mcause
  csrr  a1, mepc
  csrr  a2, mtval
  la    sp, __stack_top
  call  tile_trap_handler
4:
  j     4b
