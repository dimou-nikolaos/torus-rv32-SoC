// riscv-tests environment for a Torus tile (replaces env/p/riscv_test.h).
// Pass: write 0 to the console exit register. Fail: write (TESTNUM << 1) | 1.
// Any unexpected trap fails with 0x8000 | mcause.
#pragma once

#include "encoding.h"

#define TORUS_EXIT 0x10003004

#define RVTEST_RV32U .macro init; .endm
#define RVTEST_RV64U RVTEST_RV32U
#define RVTEST_RV32M .macro init; .endm

#define TESTNUM gp

#define RVTEST_CODE_BEGIN       \
  .section .text.init;          \
  .align 6;                     \
  .globl _start;                \
_start:                         \
  la t0, trap_vector;           \
  csrw mtvec, t0;               \
  li TESTNUM, 0;                \
  j test_start;                 \
  .align 7; /* mtvec[6:0] are not writable on CV32E40X */ \
trap_vector:                    \
  csrr a0, mcause;              \
  li t0, 0x8000;                \
  or a0, a0, t0;                \
  li t0, TORUS_EXIT;            \
  sw a0, 0(t0);                 \
1: j 1b;                        \
test_start:                     \
  init;

#define RVTEST_CODE_END         \
  unimp

#define RVTEST_PASS             \
  fence;                        \
  li t0, TORUS_EXIT;            \
  sw zero, 0(t0);               \
1: j 1b;

#define RVTEST_FAIL             \
  fence;                        \
1: beqz TESTNUM, 1b;            \
  sll a0, TESTNUM, 1;           \
  or a0, a0, 1;                 \
  li t0, TORUS_EXIT;            \
  sw a0, 0(t0);                 \
2: j 2b;

#define EXTRA_DATA

#define RVTEST_DATA_BEGIN       \
  EXTRA_DATA                    \
  .data;                        \
  .align 4;                     \
  .global begin_signature;      \
begin_signature:

#define RVTEST_DATA_END .align 4; .global end_signature; end_signature:
