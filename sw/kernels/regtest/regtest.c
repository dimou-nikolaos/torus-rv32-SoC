// Tile control, counter bank and MMIO decode test, run from the core.
// The testbench checks the remote side (COORD, CTRL, counters) itself.
#include <stdio.h>

#include "tile.h"

#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL line %d: %s\n", __LINE__, #c);         \
      return __LINE__;                                    \
    }                                                     \
  } while (0)

static volatile int expect_fault;

// The last check provokes a bus error: a load from an unmapped MMIO address.
// CV32E40X reports data bus errors as an NMI with mcause = interrupt | 1024
// (load bus fault).
void tile_trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval) {
  (void)mepc;
  (void)mtval;
  if (expect_fault && mcause == (0x80000000u | 1024)) {
    printf("expected load bus-fault NMI\n");
    tile_exit(0);
  }
  printf("unexpected trap mcause=%lu\n", (unsigned long)mcause);
  tile_exit(0x80 | (int)mcause);
}

int kernel_main(const args_t *args) {
  (void)args;
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));

  // COORD matches the hart id the tile gives the core ({y, x})
  xy_t me = my_xy();
  CHECK(me.x == (hartid & 0xf) && me.y == ((hartid >> 4) & 0xf));
  CHECK((mmio_read(TILE_TCTRL_BASE + TCTRL_COORD) >> 16) == 0);

  // CTRL reads back run = 1 while we execute
  CHECK(mmio_read(TILE_TCTRL_BASE + TCTRL_CTRL) == 1);

  // CYCLE is free-running and 64-bit reads are consistent
  uint64_t c0 = tile_cycles(), c1 = tile_cycles();
  CHECK(c1 > c0 && c1 - c0 < 100);

  // DOORBELL starts clear; write-1-to-clear leaves it clear
  CHECK(mmio_read(TILE_TCTRL_BASE + TCTRL_DOORBELL) == 0);
  mmio_write(TILE_TCTRL_BASE + TCTRL_DOORBELL, 1);
  CHECK(mmio_read(TILE_TCTRL_BASE + TCTRL_DOORBELL) == 0);

  // Counter bank: count, monotonic counters, retired <= cycles
  CHECK(perf_count() == PERF_NUM_COUNTERS);
  CHECK(mmio_read(TILE_PERF_BASE + PERF_CMD_OFFSET) == 0);
  uint64_t r0 = perf_read(PERF_RETIRED), y0 = perf_read(PERF_CYCLES);
  for (volatile int i = 0; i < 50; i++) {
  }
  uint64_t r1 = perf_read(PERF_RETIRED), y1 = perf_read(PERF_CYCLES);
  CHECK(r1 > r0 + 50 && y1 > y0 && r1 <= y1);

  // Snapshot: all counters copied in one cycle, static afterwards
  perf_cmd(PERF_CMD_SNAPSHOT);
  uint64_t s_ret = perf_read_snapshot(PERF_RETIRED), s_cyc = perf_read_snapshot(PERF_CYCLES);
  CHECK(s_ret > r1 && s_cyc > y1 && s_ret <= s_cyc);
  for (volatile int i = 0; i < 20; i++) {
  }
  CHECK(perf_read_snapshot(PERF_RETIRED) == s_ret && perf_read_snapshot(PERF_CYCLES) == s_cyc);
  CHECK(perf_read(PERF_RETIRED) > s_ret);

  // Snapshot + clear in one write: the snapshot keeps the totals, live restarts
  perf_cmd(PERF_CMD_SNAPSHOT | PERF_CMD_CLEAR);
  uint64_t after = perf_read(PERF_RETIRED);
  CHECK(perf_read_snapshot(PERF_RETIRED) > s_ret && after < 20);

  // Sub-word stores and loads through the scratchpad lanes
  static volatile uint32_t w[4];
  for (int i = 0; i < 4; i++) w[i] = 0x11111111u * (uint32_t)(i + 1);
  ((volatile uint8_t *)w)[5] = 0xab;
  ((volatile uint16_t *)w)[7] = 0xcdef;
  CHECK(w[0] == 0x11111111u && w[1] == 0x2222ab22u && w[3] == 0xcdef4444u - 0x4444u + 0x4444u);
  CHECK(w[2] == 0x33333333u);

  printf("registers OK (tile %u,%u, %lu instructions so far)\n", me.x, me.y, (unsigned long)r1);
  expect_fault = 1;
  (void)mmio_read(0x10004000u);
  printf("FAIL: unmapped MMIO load did not fault\n");
  return 1;
}
