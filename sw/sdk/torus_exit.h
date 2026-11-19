/* Program exit on a tile, shared by crt0.S and the ISA test environment
 * (SPEC.md, launch protocol, step 5). Assembler only.
 *
 *   torus_exit_seq   a0 = exit code; clobbers t0-t3; never returns
 *
 * 1. writes the exit word (0xE000_0000 | code[15:0]) at TORUS_EXIT_WORD, where
 *    the host collects it;
 * 2. waits until both NIU channels are idle (everything the program sent has
 *    been injected, its reads have landed);
 * 3. adds 1 to DONE in the I/O tile with ATOMIC_INC on channel 1 and waits for
 *    it to be injected, so the host's wait() sees this tile finish;
 * 4. writes the code to the console exit register (simulation), then spins.
 */
#pragma once

#define TORUS_EXIT_WORD   0x7FFC
#define TORUS_EXIT_MAGIC  0xE0000000
#define TORUS_IO_DONE     0x10004000

.macro torus_exit_seq
  li    t0, 0xFFFF
  and   t1, a0, t0
  li    t0, TORUS_EXIT_MAGIC
  or    t1, t1, t0
  li    t0, TORUS_EXIT_WORD
  sw    t1, 0(t0)
  li    t0, 0x10000000            /* NIU registers */
71:
  lw    t1, 0x14(t0)              /* channel 0 STATUS: busy */
  andi  t1, t1, 1
  bnez  t1, 71b
72:
  lw    t1, 0x34(t0)              /* channel 1 STATUS: busy */
  andi  t1, t1, 1
  bnez  t1, 72b
  li    t1, 0x10001014            /* GRID: I/O tile x in [23:16], y in [31:24] */
  lw    t1, 0(t1)
  srli  t2, t1, 16
  andi  t3, t2, 0xff
  srli  t2, t1, 24
  slli  t2, t2, 8
  or    t2, t2, t3
  sw    t2, 0x28(t0)              /* channel 1 DST_XY */
  li    t1, TORUS_IO_DONE
  sw    t1, 0x24(t0)              /* channel 1 DST_ADDR */
  li    t1, 0x00010002            /* ATOMIC_INC, increment 1 */
  sw    t1, 0x30(t0)              /* channel 1 CMD */
73:
  lw    t1, 0x34(t0)
  andi  t1, t1, 1
  bnez  t1, 73b
  li    t0, 0x10003004            /* console exit (simulation only) */
  sw    a0, 0(t0)
74:
  j     74b
.endm
