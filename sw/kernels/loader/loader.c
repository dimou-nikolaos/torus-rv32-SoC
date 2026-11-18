// Resident loader (SPEC.md, M4: kernel load by a software tree). Installed
// once at 0xE000 on every tile (Device::install_loader), it distributes a
// program image over the compute grid in a binomial tree instead of the host
// writing every tile through the I/O tile.
//
// For a load (Device::load_kernel_tree), the host writes the image and the
// table below only to tile 0, a 16-byte jump to 0xE000 at address 0 of every
// tile, and launches this program everywhere. Tile n > 0 waits for its image
// from its parent n - 2^floor(log2 n); every tile then sends the image and the
// table to its children n + 2^k (largest subtree first), restores the image's
// first 16 bytes (the jump stub's place), clears the exit word for the kernel
// launched next, and reports done in the I/O tile's DONE register.
//
// Ordering: a child jumps away from the stub before its parent can write
// address 0 there: it tells the parent it is ready first, and the parent sends
// only after every child has. Image and table go before the arrival
// semaphore on the same path, so the image is complete when it changes.
#include <stdint.h>

#include "tile.h"

#define MAX_SEGS 12

struct table {
  uint32_t nseg;
  uint32_t head[4];                 // the image's bytes 0..15 (address 0 holds the stub)
  uint32_t seg[MAX_SEGS][2];        // address and length, 16-byte aligned
  uint32_t pad[3];
  // Kept across loads (zero at install, never sent): from byte 128 on
  uint32_t loads;                   // loads this tile has completed
  uint32_t arrived;                 // images that arrived from the parent
  uint32_t ready;                   // ready signals from the children
  uint32_t pad2;
};

static volatile struct table tab __attribute__((section(".table")));
#define TABLE_SENT 128u   // nseg, head, seg, pad: what a parent sends
_Static_assert(__builtin_offsetof(struct table, loads) == TABLE_SENT, "table layout");

struct args;   // the loader takes nothing from the mailbox

static unsigned log2_floor(unsigned v) {
  unsigned k = 0;
  while (v >>= 1) k++;
  return k;
}

static void wait_both(void) {
  dma_wait(0);
  dma_wait(1);
}

int kernel_main(const args_t *unused) {
  (void)unused;
  unsigned n = tile_index(my_xy()), count = grid_nx() * grid_ny();
  uint32_t loads = tab.loads;
  unsigned first_k = 0;   // children are n + 2^k for k >= first_k
  if (n > 0) {
    unsigned k = log2_floor(n);
    first_k = k + 1;
    sem_inc(tile_at(n - (1u << k)), (uint32_t)&tab.ready, 1);
    sem_wait(&tab.arrived, loads + 1);
  }
  unsigned children = 0;
  for (unsigned k = first_k; n + (1u << k) < count; k++) children++;
  if (children) sem_wait(&tab.ready, (loads + 1) * children);

  // Largest subtree first: the child n + 2^k for the smallest k (its own
  // children are n + 2^k + 2^j for every j > k)
  for (int k = (int)first_k; k < (int)(first_k + children); k++) {
    xy_t child = tile_at(n + (1u << k));
    int ch = 0;
    for (unsigned s = 0; s < tab.nseg; s++) {
      uint32_t addr = tab.seg[s][0], len = tab.seg[s][1];
      if (addr == 0) {   // the stub's 16 bytes travel in the table
        addr += 16;
        len -= 16;
      }
      if (!len) continue;
      dma_wait(ch);
      dma_write(ch, addr, child, addr, len);
      ch ^= 1;
    }
    wait_both();
    dma_write(0, (uint32_t)&tab, child, (uint32_t)&tab, TABLE_SENT);
    dma_wait(0);
    sem_inc(child, (uint32_t)&tab.arrived, 1);
  }
  wait_both();
  volatile uint32_t *zero;
  __asm__("li %0, 0" : "=r"(zero));   // address 0, opaque to the compiler
  for (int i = 0; i < 4; i++) zero[i] = tab.head[i];
  tab.loads = loads + 1;
  *(volatile uint32_t *)TILE_EXIT_WORD = 0;   // the next kernel's, not exited yet
  sem_inc(io_xy(), IO_DONE, 1);
  for (;;) {
  }
}
