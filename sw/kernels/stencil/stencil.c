// 2D five-point stencil (Jacobi) on a periodic grid over the compute torus
// (SPEC.md, M4). The G x G int32 grid (G = P b) is split into one b x b block
// per tile on a P x P grid; every step replaces each point by
//   (north + south + west + east + 4 centre) >> 3   (arithmetic shift)
// with the grid wrapping around at its edges, so the torus neighbours are the
// stencil's neighbours.
//
// Step t sends the block's edges of U_t into the neighbours' halo buffers of
// parity t % 2 (rows straight from the block, columns packed first), then
// computes U_t+1 from U_t and its own halos of parity t % 2. One arrival
// semaphore per halo direction: a neighbour can be one step ahead of us (it
// needs only our edges), so a single counter could be complete while another
// neighbour's halo is still missing; per direction a neighbour is never two
// steps ahead. Halos double-buffered by parity need no free semaphores: a
// neighbour writes parity t % 2 again only at step t + 2, after it has our
// edges of step t + 1, which we send after our step t compute.
#include <stdint.h>

#include "tile.h"

struct args {
  uint32_t u[2];     // block buffers: U_t is u[t % 2]
  uint32_t halo;     // halos: halo + 0x200 * parity + 0x80 * dir (N, S, W, E), b words each
  uint32_t send;     // packed west and east columns, 0x80 apart
  uint32_t bs;       // b: block side, multiple of 4, at most 32
  uint32_t steps;
  uint32_t plane;    // NIU_PLANE_* for all of this program's traffic
  uint32_t result;   // start, cycles in the compute loops, end
};

enum { HN, HS, HW, HE };

// One semaphore per halo direction, counting the halos that arrived
static volatile uint32_t arr[4] __attribute__((section(".data"))) = {0, 0, 0, 0};

static inline uint32_t now(void) { return mmio_read(TILE_TCTRL_BASE + TCTRL_CYCLE_LO); }

static inline int32_t point(int32_t n, int32_t s, int32_t w, int32_t e, int32_t c) {
  return (n + s + w + e + 4 * c) >> 3;
}

static void step(int32_t *out, const int32_t *u, const int32_t *h, unsigned b) {
  const int32_t *hn = h, *hs = h + 32, *hw = h + 64, *he = h + 96;   // 0x80 bytes apart
  for (unsigned i = 0; i < b; i++) {
    const int32_t *row = u + i * b;
    const int32_t *up = i ? row - b : hn, *dn = i + 1 < b ? row + b : hs;
    int32_t *o = out + i * b;
    o[0] = point(up[0], dn[0], hw[i], row[1], row[0]);
    for (unsigned j = 1; j + 1 < b; j++) o[j] = point(up[j], dn[j], row[j - 1], row[j + 1], row[j]);
    o[b - 1] = point(up[b - 1], dn[b - 1], row[b - 2], he[i], row[b - 1]);
  }
}

int kernel_main(const args_t *a) {
  noc_set_plane(a->plane);
  xy_t north = neighbor(0, -1), south = neighbor(0, 1), west = neighbor(-1, 0), east = neighbor(1, 0);
  unsigned b = a->bs;
  uint32_t row_bytes = 4 * b;
  if (!tile_buffer_ok(a->u[0], b * row_bytes) || !tile_buffer_ok(a->u[1], b * row_bytes) ||
      !tile_buffer_ok(a->halo, 0x400) || !tile_buffer_ok(a->send, 0x100))
    return 3;
  int32_t *sw = (int32_t *)a->send, *se = (int32_t *)(a->send + 0x80);
  volatile uint32_t *res = (volatile uint32_t *)a->result;
  uint32_t t_compute = 0;
  res[0] = now();

  for (unsigned t = 0; t < a->steps; t++) {
    unsigned cur = t & 1;
    const int32_t *u = (const int32_t *)a->u[cur];
    uint32_t h = a->halo + 0x200 * cur;
    for (unsigned i = 0; i < b; i++) {
      sw[i] = u[i * b];
      se[i] = u[i * b + b - 1];
    }
    // Our top row is the north neighbour's south halo, and so on
    // (a channel takes a new command once the last one is injected)
    dma_write(0, (uint32_t)u, north, h + 0x80 * HS, row_bytes);
    dma_wait(1);
    dma_write(1, (uint32_t)(u + (b - 1) * b), south, h + 0x80 * HN, row_bytes);
    dma_wait(0);
    dma_write(0, (uint32_t)sw, west, h + 0x80 * HE, row_bytes);
    dma_wait(1);
    dma_write(1, (uint32_t)se, east, h + 0x80 * HW, row_bytes);
    dma_wait(0);
    // sem_inc waits for channel 1; each increment follows its data on one path
    sem_inc(north, (uint32_t)&arr[HS], 1);
    sem_inc(south, (uint32_t)&arr[HN], 1);
    sem_inc(west, (uint32_t)&arr[HE], 1);
    sem_inc(east, (uint32_t)&arr[HW], 1);
    for (int d = 0; d < 4; d++) sem_wait(&arr[d], t + 1);
    uint32_t c0 = now();
    step((int32_t *)a->u[cur ^ 1], u, (const int32_t *)h, b);
    t_compute += now() - c0;
  }
  dma_wait(1);
  res[1] = t_compute;
  res[2] = now();
  return (niu_status(0) | niu_status(1)) & NIU_STATUS_ERROR ? 1 : 0;
}
