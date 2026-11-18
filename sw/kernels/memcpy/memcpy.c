// memcpy across the torus (SPEC.md, M2 exit test). The host loads a block into
// every tile and names each tile's partner; every tile DMA-writes its block to
// its partner, then increments the partner's arrival semaphore. The increment
// follows the data on the same path, so when a tile's semaphore reaches the
// number of tiles sending to it, its incoming blocks are complete (SPEC.md,
// Ordering and completion: producer to consumer). Each tile records when it
// started, when its DMA was injected and when its data had arrived, on the
// CYCLE counter all tiles share since reset.
#include <stdint.h>

#include "tile.h"

struct args {
  uint32_t src;       // local source block
  uint32_t dst;       // destination address at the partner
  uint32_t len;       // bytes, multiple of 16
  uint32_t partner;   // x | y << 8
  uint32_t senders;   // tiles sending to this one
  uint32_t sem;       // arrival semaphore, same address on every tile, zero at launch
  uint32_t result;    // where to record t_start, t_injected, t_arrived (low words)
  uint32_t plane;     // NIU_PLANE_* for the copy and its semaphore
};

int kernel_main(const args_t *a) {
  xy_t partner = {(uint8_t)(a->partner & 0xff), (uint8_t)(a->partner >> 8)};
  volatile uint32_t *res = (volatile uint32_t *)a->result;
  noc_set_plane(a->plane);
  uint32_t t0 = (uint32_t)tile_cycles();
  dma_write(0, a->src, partner, a->dst, a->len);
  dma_wait(0);
  uint32_t t1 = (uint32_t)tile_cycles();
  sem_inc(partner, a->sem, 1);
  sem_wait((volatile uint32_t *)a->sem, a->senders);
  uint32_t t2 = (uint32_t)tile_cycles();
  res[0] = t0;
  res[1] = t1;
  res[2] = t2;
  return niu_status(0) & NIU_STATUS_ERROR ? 1 : 0;
}
