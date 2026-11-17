// Ping-pong on a 2x2 torus: tiles pair up across both rings, (0,0) with (1,1)
// and (1,0) with (0,1). The tiles in row 0 send a 64-byte message, the partner
// checks it and sends it back, ROUNDS times. The sequence number sits in the
// last flit: flits of one transfer arrive in address order on one path and
// retire in order (SPEC.md, Ordering and completion), so when it changes the
// rest of the message has landed.
#include <stdio.h>

#include "tile.h"

#define ROUNDS 64
#define WORDS 16  // 64 bytes = 4 flits; word 15 is the sequence number

// In .data so the backdoor loader initializes it before any tile runs
static volatile uint32_t inbox[WORDS] __attribute__((aligned(16), section(".data"))) = {0};
static uint32_t outbox[WORDS] __attribute__((aligned(16)));

static void make(uint32_t seq, xy_t from) {
  for (int i = 0; i < WORDS - 1; i++) outbox[i] = seq * 0x01010101u ^ (uint32_t)i << 24 ^ from.x << 4 ^ from.y;
  outbox[WORDS - 1] = seq;
}

static int check(uint32_t seq, xy_t from) {
  for (int i = 0; i < WORDS - 1; i++)
    if (inbox[i] != (seq * 0x01010101u ^ (uint32_t)i << 24 ^ from.x << 4 ^ from.y)) return 0;
  return 1;
}

int kernel_main(const args_t *args) {
  (void)args;
  xy_t me = my_xy();
  xy_t peer = {(uint8_t)(me.x ^ 1), (uint8_t)(me.y ^ 1)};
  int initiator = me.y == 0;
  uint64_t total = 0;

  for (uint32_t seq = 1; seq <= ROUNDS; seq++) {
    if (initiator) {
      make(seq, me);
      uint64_t t0 = tile_cycles();
      dma_write(0, (uint32_t)outbox, peer, (uint32_t)inbox, sizeof outbox);
      dma_wait(0);
      while (inbox[WORDS - 1] != seq) {
      }
      total += tile_cycles() - t0;
      if (!check(seq, peer)) {
        printf("FAIL: round %lu reply corrupted\n", (unsigned long)seq);
        return 2;
      }
    } else {
      while (inbox[WORDS - 1] != seq) {
      }
      if (!check(seq, peer)) {
        printf("FAIL: round %lu message corrupted\n", (unsigned long)seq);
        return 3;
      }
      make(seq, me);
      dma_write(0, (uint32_t)outbox, peer, (uint32_t)inbox, sizeof outbox);
      dma_wait(0);
    }
  }
  if (initiator)
    printf("pingpong (%u,%u)<->(%u,%u): %d rounds of %u bytes, %lu cycles per round trip\n", me.x, me.y, peer.x,
           peer.y, ROUNDS, (unsigned)sizeof outbox, (unsigned long)(total / ROUNDS));
  return 0;
}
