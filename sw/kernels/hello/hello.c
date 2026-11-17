// M0 hello-world: prints from the tile, reads its coordinates and counters.
#include <stdio.h>

#include "tile.h"

int kernel_main(const args_t *args) {
  (void)args;
  xy_t me = my_xy();
  printf("Hello from tile (%u, %u)\n", me.x, me.y);
  uint64_t c0 = tile_cycles();
  volatile uint32_t acc = 0;
  for (int i = 0; i < 100; i++) acc += (uint32_t)i * 3u;
  uint64_t c1 = tile_cycles();
  printf("sum=%lu in %lu cycles; retired so far=%lu\n", (unsigned long)acc,
         (unsigned long)(c1 - c0), (unsigned long)perf_read(PERF_RETIRED));
  return acc == 14850 ? 0 : 1;
}
