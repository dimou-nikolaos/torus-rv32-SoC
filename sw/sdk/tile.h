// Kernel SDK for compute tiles (SPEC.md, Software stack).
// M0: coordinates, cycle counter, performance counters, console and exit.
// The NoC calls (DMA, semaphores) are declared now and arrive with the NIU in M1/M2.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------- memory map
#define TILE_SPM_BASE      0x00000000u
#define TILE_SYNC_BASE     0x0000F000u
#define TILE_MAILBOX_BASE  0x0000FF00u
#define TILE_NIU_BASE      0x10000000u
#define TILE_TCTRL_BASE    0x10001000u
#define TILE_PERF_BASE     0x10002000u
#define TILE_CONSOLE_BASE  0x10003000u

#define TCTRL_COORD     0x00
#define TCTRL_CTRL      0x04
#define TCTRL_CYCLE_LO  0x08
#define TCTRL_CYCLE_HI  0x0C
#define TCTRL_DOORBELL  0x10

#define CONSOLE_TX      0x00
#define CONSOLE_EXIT    0x04

// Counter bank (rtl/perf/counter_bank.sv): counters 0 and 1 are fixed (cycles,
// retired instructions); counters 2..9 count the event their select names.
enum {
  PERF_CYCLES = 0,
  PERF_RETIRED = 1,
  PERF_PROG_0 = 2,           // first programmable counter
  PERF_NUM_COUNTERS = 10,
  // Programmable counters as selected after reset
  PERF_C_FETCH_STALL = 2,
  PERF_C_DATA_STALL = 3,
  PERF_C_NIU_TX = 4,
  PERF_C_NIU_RX = 5,
  PERF_C_NIU_ERR = 6,
  PERF_C_R0_FLIT_L = 7,
  PERF_C_R0_FLIT_X = 8,
  PERF_C_R0_FLIT_Y = 9,
  // Register offsets
  PERF_SEL_BASE = 0x400,
  PERF_SNAP_BASE = 0x800,
  PERF_NUM_OFFSET = 0xFF0,
  PERF_CMD_OFFSET = 0xFF4,
  PERF_NEV_OFFSET = 0xFF8,
  PERF_CMD_SNAPSHOT = 1u << 0,
  PERF_CMD_CLEAR = 1u << 1,
};

// Event numbers for the select registers (rtl/pkg/tile_pkg.sv, perf_event_e)
enum {
  PERF_EV_CYCLES = 0,
  PERF_EV_RETIRED = 1,
  PERF_EV_FETCH_STALL = 2,
  PERF_EV_DATA_STALL = 3,
  PERF_EV_DENIED_0 = 4,     // 4..10 per arbiter port
  PERF_EV_PROMOTED_0 = 11,  // 11..17 per arbiter port
  PERF_EV_NIU_TX = 18,
  PERF_EV_NIU_RX = 19,
  PERF_EV_NIU_ERR = 20,
  PERF_EV_R0_FLIT = 21,     // 21..23 NoC0 router flits per output (L, X, Y)
  PERF_EV_R0_CSTALL = 24,   // 24..26 per output: waiting, no credit
  PERF_EV_R0_ARBLOSS = 27,  // 27..29 per input: lost arbitration
  PERF_EV_R0_BHOLD = 30,    // 30..32 per output: entering flit held by the bubble rule
  PERF_NUM_EVENTS = 33,
};

static inline uint32_t mmio_read(uint32_t addr) { return *(volatile uint32_t *)addr; }
static inline void mmio_write(uint32_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }

// ------------------------------------------------------------ coordinates
typedef struct { uint8_t x, y; } xy_t;

static inline xy_t my_xy(void) {
  uint32_t c = mmio_read(TILE_TCTRL_BASE + TCTRL_COORD);
  xy_t r = {(uint8_t)(c & 0xff), (uint8_t)((c >> 8) & 0xff)};
  return r;
}

// ----------------------------------------------------------------- timing
static inline uint64_t tile_cycles(void) {
  uint32_t lo = mmio_read(TILE_TCTRL_BASE + TCTRL_CYCLE_LO);   // latches HI
  uint32_t hi = mmio_read(TILE_TCTRL_BASE + TCTRL_CYCLE_HI);
  return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t perf_read(unsigned idx) {
  uint32_t lo = mmio_read(TILE_PERF_BASE + 8 * idx);           // latches HI
  uint32_t hi = mmio_read(TILE_PERF_BASE + 8 * idx + 4);
  return ((uint64_t)hi << 32) | lo;
}

static inline unsigned perf_count(void) { return mmio_read(TILE_PERF_BASE + PERF_NUM_OFFSET); }
static inline unsigned perf_events(void) { return mmio_read(TILE_PERF_BASE + PERF_NEV_OFFSET); }

// Make programmable counter `counter` (PERF_PROG_0..) count `event` (PERF_EV_*).
// Its value is kept; clear the counters to start from zero.
static inline void perf_select(unsigned counter, unsigned event) {
  mmio_write(TILE_PERF_BASE + PERF_SEL_BASE + 4 * (counter - PERF_PROG_0), event);
}

// Snapshot all counters at once and/or clear them (PERF_CMD_SNAPSHOT | PERF_CMD_CLEAR).
static inline void perf_cmd(uint32_t cmd) { mmio_write(TILE_PERF_BASE + PERF_CMD_OFFSET, cmd); }

static inline uint64_t perf_read_snapshot(unsigned idx) {
  uint32_t lo = mmio_read(TILE_PERF_BASE + PERF_SNAP_BASE + 8 * idx);
  uint32_t hi = mmio_read(TILE_PERF_BASE + PERF_SNAP_BASE + 8 * idx + 4);
  return ((uint64_t)hi << 32) | lo;
}

// ------------------------------------------------------- console and exit
static inline void console_putc(char c) { mmio_write(TILE_CONSOLE_BASE + CONSOLE_TX, (uint8_t)c); }
void console_puts(const char *s);
void tile_exit(int code) __attribute__((noreturn));

// ------------------------------------------------------------------ NoC
// NIU registers, per DMA channel (SPEC.md, Tile memory map and NIU registers)
#define NIU_CH(c)      (TILE_NIU_BASE + 0x20u * (uint32_t)(c))
#define NIU_SRC        0x00
#define NIU_DST_ADDR   0x04
#define NIU_DST_XY     0x08
#define NIU_LEN        0x0C
#define NIU_CMD        0x10
#define NIU_STATUS     0x14
#define NIU_OP_WRITE   0u
#define NIU_OP_READ_REQ 1u
#define NIU_OP_ATOMIC_INC 2u
#define NIU_STATUS_BUSY  (1u << 0)
#define NIU_STATUS_ERROR (1u << 1)
#define NIU_STATUS_DONE(s) ((s) >> 16)   // completed commands, modulo 2^16

static inline uint32_t niu_status(int ch) { return mmio_read(NIU_CH(ch) + NIU_STATUS); }

// M1: dma_write and dma_wait. The rest arrives with the NIU features in M2/M3.
// dma_write: src, dst_addr and len multiples of 16; returns at once, the
// channel stays busy until every flit has been injected.
void dma_write(int ch, uint32_t src, xy_t dst, uint32_t dst_addr, uint32_t len);
void dma_read(xy_t src, uint32_t src_addr, uint32_t dst, uint32_t len);  // channel 0
void dma_wait(int ch);   // until the channel's command has finished (writes: injected)
void sem_inc(xy_t dst, uint32_t sem_addr, uint32_t n);
void sem_wait(volatile uint32_t *sem, uint32_t value);
void noc_fence(xy_t dst);
xy_t neighbor(int dx, int dy);
xy_t io_xy(void);

// Kernel entry, called by crt0 with the mailbox address.
typedef struct args args_t;
int kernel_main(const args_t *args);

#ifdef __cplusplus
}
#endif
