// All-reduce over the compute torus (SPEC.md, M4): every tile holds a vector
// of int32 (len bytes); afterwards every tile holds the element-wise sum over
// all tiles, modulo 2^32.
//
// Reduce along each row towards x = 0, then along column 0 towards y = 0, as
// binary trees: at level d (1, 2, 4, ...) the tile at x = 2dk + d sends its
// partial sum to x = 2dk, which adds it. Then the sum travels back down the
// same trees: column 0 first, then the rows. Each level has its own receive
// buffer and arrival semaphore, so a fast sender never overwrites a buffer
// still being added; a broadcast lands in the receiver's vector, which it
// last read as the source of its own (completed) send.
#include <stdint.h>

#include "tile.h"

struct args {
  uint32_t vec;      // the vector, len bytes; the sum at exit
  uint32_t recv;     // receive buffers: recv + stride * (4 * dim + level), up to 16x16
  uint32_t stride;   // len rounded up to 16
  uint32_t len;      // bytes, multiple of 16
  uint32_t plane;    // NIU_PLANE_* for all of this program's traffic
  uint32_t result;   // start, cycles in the add loops, end
};

// Arrivals: [8 * dim + level] for the reduce, [16] for the broadcast
static volatile uint32_t sem[17] __attribute__((section(".data"))) = {0};

static inline uint32_t now(void) { return mmio_read(TILE_TCTRL_BASE + TCTRL_CYCLE_LO); }

static uint32_t add_into(uint32_t *v, const uint32_t *r, unsigned n) {
  uint32_t t0 = now();
  for (unsigned i = 0; i < n; i += 4) {
    v[i] += r[i];
    v[i + 1] += r[i + 1];
    v[i + 2] += r[i + 2];
    v[i + 3] += r[i + 3];
  }
  return now() - t0;
}

static void send(xy_t to, uint32_t src, uint32_t dst, uint32_t len, unsigned s) {
  dma_write(0, src, to, dst, len);
  dma_wait(0);
  sem_inc(to, (uint32_t)&sem[s], 1);
}

int kernel_main(const args_t *a) {
  noc_set_plane(a->plane);
  xy_t me = my_xy();
  unsigned pos[2] = {me.x, me.y}, size[2] = {grid_nx(), grid_ny()};
  uint32_t *v = (uint32_t *)a->vec;
  unsigned n = a->len / 4;
  uint32_t t_add = 0;
  volatile uint32_t *res = (volatile uint32_t *)a->result;
  res[0] = now();

  // Reduce: rows (dim 0), then column 0 (dim 1)
  for (unsigned dim = 0; dim < 2; dim++) {
    if (dim == 1 && me.x != 0) break;
    for (unsigned d = 1, lvl = 0; d < size[dim]; d *= 2, lvl++) {
      unsigned s = 8 * dim + lvl;
      uint32_t buf = a->recv + a->stride * (4 * dim + lvl);
      if (pos[dim] % (2 * d) == d) {
        send(dim ? neighbor(0, -(int)d) : neighbor(-(int)d, 0), a->vec, buf, a->len, s);
        break;
      }
      if (pos[dim] + d < size[dim]) {
        sem_wait(&sem[s], 1);
        t_add += add_into(v, (const uint32_t *)buf, n);
      }
    }
  }

  // Broadcast: column 0 (dim 1), then rows (dim 0), from the top level down
  for (int dim = 1; dim >= 0; dim--) {
    if (dim == 1 && me.x != 0) continue;
    unsigned top = 1;
    while (top < size[dim]) top *= 2;
    int have = pos[dim] == 0;   // the root of this dimension's tree has the sum
    for (unsigned d = top / 2; d >= 1; d /= 2) {
      if (!have && pos[dim] % (2 * d) == d) {
        sem_wait(&sem[16], 1);   // every tile but (0, 0) receives the sum exactly once
        have = 1;
      } else if (have && pos[dim] % (2 * d) == 0 && pos[dim] + d < size[dim]) {
        send(dim ? neighbor(0, (int)d) : neighbor((int)d, 0), a->vec, a->vec, a->len, 16);
      }
    }
  }
  res[1] = t_add;
  res[2] = now();
  return (niu_status(0) | niu_status(1)) & NIU_STATUS_ERROR ? 1 : 0;
}
