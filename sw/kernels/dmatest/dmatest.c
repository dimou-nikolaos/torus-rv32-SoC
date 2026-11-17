// NIU DMA test on one tile (works on any grid): loopback writes into the tile's
// own scratchpad through the router, both channels at once, a 16 KB transfer, and
// the error cases of the command checks.
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

#define BIG 16384u
static uint8_t src_a[BIG] __attribute__((aligned(16)));
static uint8_t dst_a[BIG] __attribute__((aligned(16)));
static uint8_t src_b[1024] __attribute__((aligned(16)));
static uint8_t dst_b[1024] __attribute__((aligned(16)));

static void fill(uint8_t *p, uint32_t n, uint32_t seed) {
  for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed * 131u + i * 7u + (i >> 8));
}

// Loopback flits are posted: wait until the last word has landed.
static void wait_last(volatile uint8_t *dst, const uint8_t *src, uint32_t len) {
  while (memcmp((const void *)(dst + len - 16), src + len - 16, 16) != 0) {
  }
}

int kernel_main(const args_t *args) {
  (void)args;
  xy_t me = my_xy();

  // 1. A single 16-byte flit, then 1 KB, to ourselves
  fill(src_b, 1024, 1);
  memset(dst_b, 0, 1024);
  dma_write(0, (uint32_t)src_b, me, (uint32_t)dst_b, 16);
  dma_wait(0);
  wait_last(dst_b, src_b, 16);
  CHECK(memcmp(dst_b, src_b, 16) == 0 && dst_b[16] == 0);
  dma_write(1, (uint32_t)src_b, me, (uint32_t)dst_b, 1024);
  dma_wait(1);
  wait_last(dst_b, src_b, 1024);
  CHECK(memcmp(dst_b, src_b, 1024) == 0);
  CHECK(!(niu_status(1) & NIU_STATUS_ERROR));

  // 2. Both channels at once: 16 KB on channel 0, 1 KB on channel 1
  fill(src_a, BIG, 2);
  fill(src_b, 1024, 3);
  memset(dst_a, 0, BIG);
  uint32_t done0 = NIU_STATUS_DONE(niu_status(0));
  uint64_t t0 = tile_cycles();
  dma_write(0, (uint32_t)src_a, me, (uint32_t)dst_a, BIG);
  dma_write(1, (uint32_t)src_b, me, (uint32_t)dst_b, 1024);
  dma_wait(0);
  uint64_t t1 = tile_cycles();
  dma_wait(1);
  wait_last(dst_a, src_a, BIG);
  wait_last(dst_b, src_b, 1024);
  CHECK(memcmp(dst_a, src_a, BIG) == 0);
  CHECK(memcmp(dst_b, src_b, 1024) == 0);
  CHECK(NIU_STATUS_DONE(niu_status(0)) == ((done0 + 1) & 0xffff));
  printf("16 KB loopback injected in %lu cycles (%lu flits)\n", (unsigned long)(t1 - t0),
         (unsigned long)(BIG / 16));

  // 3. Command errors: each completes at once with the error bit, never hangs
  struct { uint32_t src, dst_addr, xy, len, cmd; } bad[] = {
    {(uint32_t)src_b + 4, (uint32_t)dst_b, 0, 16, NIU_OP_WRITE},       // misaligned source
    {(uint32_t)src_b, (uint32_t)dst_b + 8, 0, 16, NIU_OP_WRITE},       // misaligned destination
    {(uint32_t)src_b, (uint32_t)dst_b, 0, 20, NIU_OP_WRITE},           // length not a multiple of 16
    {0xFFF0u, (uint32_t)dst_b, 0, 32, NIU_OP_WRITE},                   // source past the scratchpad
    {(uint32_t)src_b, (uint32_t)dst_b, 0x0F0F, 16, NIU_OP_WRITE},      // outside the grid
    {(uint32_t)src_b, (uint32_t)dst_b, 0x10000, 16, NIU_OP_WRITE},     // upper bits set
    {(uint32_t)src_b, (uint32_t)dst_b, 0, 16, NIU_OP_READ_REQ},        // not before M2
    {(uint32_t)src_b, (uint32_t)dst_b, 0, 16, NIU_OP_WRITE | 2u << 3}, // NoC1 forced: not before M3
  };
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    uint32_t base = NIU_CH(0), before = NIU_STATUS_DONE(niu_status(0));
    mmio_write(base + NIU_SRC, bad[i].src);
    mmio_write(base + NIU_DST_ADDR, bad[i].dst_addr);
    mmio_write(base + NIU_DST_XY, bad[i].xy ? bad[i].xy : ((uint32_t)me.x | (uint32_t)me.y << 8));
    mmio_write(base + NIU_LEN, bad[i].len);
    mmio_write(base + NIU_CMD, bad[i].cmd);
    uint32_t s = niu_status(0);
    if (!(s & NIU_STATUS_ERROR) || (s & NIU_STATUS_BUSY) || NIU_STATUS_DONE(s) != ((before + 1) & 0xffff)) {
      printf("FAIL: bad command %u: status 0x%08lx\n", i, (unsigned long)s);
      return 100 + (int)i;
    }
  }
  // A router-only stop in the I/O row has no NIU
  if (grid_nx() > 1) {
    uint32_t before = NIU_STATUS_DONE(niu_status(0));
    xy_t stop = {(uint8_t)(grid_nx() - 1), (uint8_t)grid_ny()};
    dma_write(0, (uint32_t)src_b, stop, (uint32_t)dst_b, 16);
    uint32_t s = niu_status(0);
    CHECK((s & NIU_STATUS_ERROR) && NIU_STATUS_DONE(s) == ((before + 1) & 0xffff));
  }
  // A good command clears the error bit
  dma_write(0, (uint32_t)src_b, me, (uint32_t)dst_b, 16);
  dma_wait(0);
  CHECK(!(niu_status(0) & NIU_STATUS_ERROR));

  // 4. CMD while busy is rejected, the running command is unaffected
  fill(src_a, BIG, 4);
  dma_write(0, (uint32_t)src_a, me, (uint32_t)dst_a, BIG);
  mmio_write(NIU_CH(0) + NIU_CMD, NIU_OP_WRITE);
  CHECK(niu_status(0) & NIU_STATUS_ERROR);
  dma_wait(0);
  wait_last(dst_a, src_a, BIG);
  CHECK(memcmp(dst_a, src_a, BIG) == 0);

  printf("dmatest OK on tile (%u,%u): %lu flits sent, %lu received\n", me.x, me.y,
         (unsigned long)perf_read(PERF_C_NIU_TX), (unsigned long)perf_read(PERF_C_NIU_RX));
  return perf_read(PERF_C_NIU_ERR) == 0 ? 0 : 1;
}
