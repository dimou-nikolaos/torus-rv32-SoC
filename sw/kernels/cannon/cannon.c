// Cannon's algorithm on the compute torus (SPEC.md, M3 exit test): C = A B for
// N x N int32 matrices (arithmetic modulo 2^32), one b x b block of each per
// tile on a P x P grid, b = N / P. The host loads the blocks already skewed:
// tile (x, y) starts with A(y, (x + y) mod P) and B((x + y) mod P, x).
//
// Step k = 0 .. P-1: C += A_cur B_cur; except after the last step, A_cur moves
// to the west neighbor and B_cur to the north neighbor, into the buffer they
// do not compute on. The shifts run on the DMA channels while the core
// computes (double buffering); semaphores carry the hand-over:
//   a_arr, b_arr    blocks that arrived here (from east, from south)
//   a_free, b_free  times the west (north) neighbor freed the buffer we send into
// A send at step k waits until the receiver has finished step k-1 with that
// buffer; data and its semaphore take one path, so the data is in place when
// the semaphore changes (SPEC.md, Ordering and completion). Every buffer sits
// at an address the host passes (the same on every tile), outside .bss, and
// the semaphores are in .data: a neighbor may write here before crt0 runs.
#include <stdint.h>

#include "tile.h"

struct args {
  uint32_t a[2], b[2];  // block buffers: [k % 2] is current at step k
  uint32_t c;           // C block, zero at launch
  uint32_t bs;          // b: elements per block side
  uint32_t plane;       // NIU_PLANE_* for all of this program's traffic
  uint32_t result;      // per step k at [4k .. 4k+3]: start, compute begins, compute done, ready
};

static volatile uint32_t a_arr __attribute__((section(".data"))) = 0;
static volatile uint32_t b_arr __attribute__((section(".data"))) = 0;
static volatile uint32_t a_free __attribute__((section(".data"))) = 0;
static volatile uint32_t b_free __attribute__((section(".data"))) = 0;

static inline uint32_t now(void) { return mmio_read(TILE_TCTRL_BASE + TCTRL_CYCLE_LO); }

// c += a b, n x n, row-major
static void block_mac(uint32_t *c, const uint32_t *a, const uint32_t *b, unsigned n) {
  for (unsigned i = 0; i < n; i++)
    for (unsigned j = 0; j < n; j++) {
      const uint32_t *ap = a + i * n, *bp = b + j;
      uint32_t acc = c[i * n + j];
      for (unsigned k = 0; k < n; k++) acc += ap[k] * bp[k * n];
      c[i * n + j] = acc;
    }
}

int kernel_main(const args_t *a) {
  unsigned p = grid_nx();
  if (grid_ny() != p) return 2;   // Cannon needs a square grid
  noc_set_plane(a->plane);
  xy_t west = neighbor(-1, 0), east = neighbor(1, 0), north = neighbor(0, -1), south = neighbor(0, 1);
  uint32_t bytes = a->bs * a->bs * 4;
  volatile uint32_t *res = (volatile uint32_t *)a->result;

  for (unsigned k = 0; k < p; k++) {
    unsigned cur = k & 1, nxt = cur ^ 1;
    int shift = k + 1 < p;
    res[4 * k] = now();
    if (shift) {
      sem_wait(&a_free, k);
      sem_wait(&b_free, k);
      dma_write(0, a->a[cur], west, a->a[nxt], bytes);
      dma_write(1, a->b[cur], north, a->b[nxt], bytes);
    }
    res[4 * k + 1] = now();
    block_mac((uint32_t *)a->c, (const uint32_t *)a->a[cur], (const uint32_t *)a->b[cur], a->bs);
    res[4 * k + 2] = now();
    if (shift) {
      dma_wait(0);
      sem_inc(west, (uint32_t)&a_arr, 1);   // after channel 1 (B) too
      sem_inc(north, (uint32_t)&b_arr, 1);
      sem_wait(&a_arr, k + 1);
      sem_wait(&b_arr, k + 1);
      sem_inc(east, (uint32_t)&a_free, 1);  // our current buffers are free
      sem_inc(south, (uint32_t)&b_free, 1);
    }
    res[4 * k + 3] = now();
  }
  return (niu_status(0) | niu_status(1)) & NIU_STATUS_ERROR ? 1 : 0;
}
