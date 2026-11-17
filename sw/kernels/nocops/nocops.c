// NoC operations on any grid (SPEC.md, NoC packet format; Ordering and
// completion): every tile reads every tile's scratchpad and tile control with
// dma_read, all tiles increment one semaphore with ATOMIC_INC (one of them
// with the doorbell), noc_fence orders a write before a third tile looks, and
// the new command checks set the error bit.
#include <stdio.h>
#include <string.h>

#include "tile.h"

#define CHECK(c)                                    \
  do {                                              \
    if (!(c)) {                                     \
      printf("FAIL line %d: %s\n", __LINE__, #c);   \
      return __LINE__;                              \
    }                                               \
  } while (0)

#define PAT 1024u
#define ROUNDS 10

static uint32_t pattern[PAT / 4] __attribute__((aligned(16)));
static uint32_t inbuf[PAT / 4] __attribute__((aligned(16)));
static uint32_t regbuf[8] __attribute__((aligned(16)));
static uint32_t mailslot[4] __attribute__((aligned(16), section(".data"))) = {0};
static uint32_t outslot[4] __attribute__((aligned(16)));
// Semaphores in .data: zero in the image, so no tile clears them at run time
static volatile uint32_t ready __attribute__((section(".data"))) = 0;
static volatile uint32_t total __attribute__((section(".data"))) = 0;
static volatile uint32_t fenced __attribute__((section(".data"))) = 0;
static volatile uint32_t finished __attribute__((section(".data"))) = 0;

static uint32_t pat(xy_t t, uint32_t i) { return (uint32_t)t.x << 28 ^ (uint32_t)t.y << 24 ^ i * 0x9e3779b9u; }

int kernel_main(const args_t *args) {
  (void)args;
  xy_t me = my_xy();
  unsigned n = grid_nx() * grid_ny(), idx = tile_index(me);

  // 1. Publish a pattern, then a barrier: everyone has published
  for (uint32_t i = 0; i < PAT / 4; i++) pattern[i] = pat(me, i);
  for (unsigned t = 0; t < n; t++) sem_inc(tile_at(t), (uint32_t)&ready, 1);
  sem_wait(&ready, n);

  // 2. Read every tile's pattern: whole, and one 16-byte piece
  for (unsigned t = 0; t < n; t++) {
    xy_t src = tile_at(t);
    memset(inbuf, 0, sizeof inbuf);
    dma_read(src, (uint32_t)pattern, (uint32_t)inbuf, PAT);
    dma_wait(0);
    CHECK(!(niu_status(0) & NIU_STATUS_ERROR));
    for (uint32_t i = 0; i < PAT / 4; i++)
      if (inbuf[i] != pat(src, i)) {
        printf("FAIL: read from (%u,%u) word %lu: %08lx\n", src.x, src.y, (unsigned long)i, (unsigned long)inbuf[i]);
        return 2;
      }
    memset(inbuf, 0, 32);
    dma_read(src, (uint32_t)&pattern[16], (uint32_t)inbuf, 16);
    dma_wait(0);
    CHECK(inbuf[0] == pat(src, 16) && inbuf[3] == pat(src, 19) && inbuf[4] == 0);
  }

  // 3. Remote registers: COORD, CTRL and GRID of every tile; our own counters
  for (unsigned t = 0; t < n; t++) {
    xy_t src = tile_at(t);
    dma_read(src, TILE_TCTRL_BASE, (uint32_t)regbuf, 32);
    dma_wait(0);
    CHECK(regbuf[0] == ((uint32_t)src.y << 8 | src.x));
    CHECK(regbuf[1] == 1);
    CHECK(regbuf[5] == tile_grid());
  }
  uint64_t before = perf_read(PERF_RETIRED);
  dma_read(me, TILE_PERF_BASE, (uint32_t)regbuf, 16);
  dma_wait(0);
  uint64_t cycles = (uint64_t)regbuf[1] << 32 | regbuf[0], retired = (uint64_t)regbuf[3] << 32 | regbuf[2];
  CHECK(retired > before && retired < cycles);

  // 4. Atomics: everyone adds idx + 1 to tile 0's total, ROUNDS times; the
  //    last increment of each tile rings tile 0's doorbell
  xy_t t0 = tile_at(0);
  for (int r = 0; r < ROUNDS; r++) {
    if (r == ROUNDS - 1) sem_inc_doorbell(t0, (uint32_t)&total, idx + 1);
    else sem_inc(t0, (uint32_t)&total, idx + 1);
  }
  if (idx == 0) {
    uint32_t want = ROUNDS * n * (n + 1) / 2;
    sem_wait(&total, want);
    for (volatile int i = 0; i < 200; i++) {
    }
    CHECK(total == want);
    CHECK(mmio_read(TILE_TCTRL_BASE + TCTRL_DOORBELL) == 1);
    mmio_write(TILE_TCTRL_BASE + TCTRL_DOORBELL, 1);
    CHECK(mmio_read(TILE_TCTRL_BASE + TCTRL_DOORBELL) == 0);
  }

  // 5. Fence: write to the next tile, fence, tell the one after; that tile
  //    reads the written words from the next tile and must see them
  xy_t next = tile_at((idx + 1) % n), after = tile_at((idx + 2) % n);
  for (int i = 0; i < 4; i++) outslot[i] = 0xf0000000u | idx << 8 | (uint32_t)i;
  dma_write(0, (uint32_t)outslot, next, (uint32_t)mailslot, 16);
  dma_wait(0);
  noc_fence(next);
  sem_inc(after, (uint32_t)&fenced, 1);
  sem_wait(&fenced, 1);
  unsigned writer = (idx + 2 * n - 2) % n;
  dma_read(tile_at((writer + 1) % n), (uint32_t)mailslot, (uint32_t)inbuf, 16);
  dma_wait(0);
  CHECK(inbuf[0] == (0xf0000000u | writer << 8) && inbuf[3] == (0xf0000000u | writer << 8 | 3));

  // 6. Command checks
  uint32_t ch1 = NIU_CH(1);
  mmio_write(ch1 + NIU_SRC, (uint32_t)inbuf);
  mmio_write(ch1 + NIU_DST_ADDR, (uint32_t)pattern);
  mmio_write(ch1 + NIU_DST_XY, (uint32_t)me.x | (uint32_t)me.y << 8);
  mmio_write(ch1 + NIU_LEN, 16);
  mmio_write(ch1 + NIU_CMD, NIU_OP_READ_REQ);            // READ_REQ on channel 1
  CHECK(niu_status(1) & NIU_STATUS_ERROR);
  dma_read(io_xy(), 0, (uint32_t)inbuf, 16);             // the I/O tile serves no reads
  CHECK(niu_status(0) & NIU_STATUS_ERROR);
  if (grid_nx() > 1) {
    xy_t stop = {1, (uint8_t)grid_ny()};
    dma_read(stop, 0, (uint32_t)inbuf, 16);              // router-only stop
    CHECK(niu_status(0) & NIU_STATUS_ERROR);
  }
  dma_read(me, (uint32_t)pattern, (uint32_t)inbuf, 0);   // zero length: done at once, no error
  CHECK(!(niu_status(0) & (NIU_STATUS_ERROR | NIU_STATUS_BUSY)));

  // Stay until every tile is done reading from us
  for (unsigned t = 0; t < n; t++) sem_inc(tile_at(t), (uint32_t)&finished, 1);
  sem_wait(&finished, n);
  printf("nocops OK on tile (%u,%u) of %u\n", me.x, me.y, n);
  return 0;
}
