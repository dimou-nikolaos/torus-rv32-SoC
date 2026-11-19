// Kernel SDK for compute tiles (SPEC.md, Software stack): coordinates, cycle
// counter, performance counters, console and exit, DMA, semaphores, fences.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------- memory map
// The same at every scratchpad size (32 or 64 KB; tile_spm_bytes()): the
// program region, then the loader, sync words and the mailbox in the low 32 KB;
// 64 KB adds 0x8000..0xFFFF for buffers.
#define TILE_SPM_BASE      0x00000000u
#define TILE_PROGRAM_END   0x00006800u   // end of the program region (code, data, stack, heap)
#define TILE_LOADER_BASE   0x00006800u   // the resident loader (sw/kernels/loader)
#define TILE_SYNC_BASE     0x00007400u
#define TILE_MAILBOX_BASE  0x00007F00u   // args from the host: 0x7F00..0x7FEF
#define TILE_EXIT_WORD     0x00007FFCu   // 0xE000_0000 | exit code, written at exit
#define IO_SEM(k)          (0x10004000u + 4u * (unsigned)(k))   // host semaphores in the I/O tile
#define IO_DONE            IO_SEM(0)
#define TILE_NIU_BASE      0x10000000u
#define TILE_TCTRL_BASE    0x10001000u
#define TILE_PERF_BASE     0x10002000u
#define TILE_CONSOLE_BASE  0x10003000u

#define TCTRL_COORD     0x00
#define TCTRL_CTRL      0x04
#define TCTRL_CYCLE_LO  0x08
#define TCTRL_CYCLE_HI  0x0C
#define TCTRL_DOORBELL  0x10
#define TCTRL_GRID      0x14   // compute NX [7:0], NY [15:8], I/O tile x [23:16], y [31:24]
#define TCTRL_SPM       0x18   // scratchpad bytes

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
  PERF_C_NIU_TX0 = 4,
  PERF_C_NIU_TX1 = 5,
  PERF_C_NIU_RX0 = 6,
  PERF_C_NIU_RX1 = 7,
  PERF_C_NIU_ERR = 8,
  PERF_C_CORE_D_DENIED = 9,
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
  PERF_EV_NIU_TX0 = 18,     // flits injected on NoC0
  PERF_EV_NIU_TX1 = 19,     // ... on NoC1
  PERF_EV_NIU_RX0 = 20,     // flits retired from NoC0
  PERF_EV_NIU_RX1 = 21,     // ... from NoC1
  PERF_EV_NIU_ERR = 22,     // cycles with a flit dropped
  PERF_EV_R0_FLIT = 23,     // 23..25 NoC0 router flits per output (L, X, Y)
  PERF_EV_R0_CSTALL = 26,   // 26..28 per output: waiting, no credit
  PERF_EV_R0_ARBLOSS = 29,  // 29..31 per input: lost arbitration
  PERF_EV_R0_BHOLD = 32,    // 32..34 per output: entering flit held by the bubble rule
  PERF_EV_R1_FLIT = 35,     // 35..46 the same for the NoC1 router
  PERF_EV_R1_CSTALL = 38,
  PERF_EV_R1_ARBLOSS = 41,
  PERF_EV_R1_BHOLD = 44,
  PERF_NUM_EVENTS = 47,
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

// The compute grid is NX x NY at router coordinates x in [0, NX), y in [0, NY);
// the I/O row (y = NY) holds the I/O tile and router-only stops.
static inline uint32_t tile_grid(void) { return mmio_read(TILE_TCTRL_BASE + TCTRL_GRID); }
static inline unsigned grid_nx(void) { return tile_grid() & 0xff; }
static inline unsigned grid_ny(void) { return (tile_grid() >> 8) & 0xff; }

static inline xy_t io_xy(void) {
  uint32_t g = tile_grid();
  xy_t r = {(uint8_t)((g >> 16) & 0xff), (uint8_t)(g >> 24)};
  return r;
}

static inline uint32_t tile_spm_bytes(void) { return mmio_read(TILE_TCTRL_BASE + TCTRL_SPM); }

// The program's footprint is [0, tile_program_end()): code, data, .bss and the
// stack (sw/sdk/link.ld). Buffers the host places must start at or above it.
extern char __program_end[];
static inline uint32_t tile_program_end(void) { return (uint32_t)__program_end; }

// A buffer of len bytes at addr lies in free scratchpad: in the program region
// above the program, or in the upper 32 KB of a 64 KB scratchpad.
static inline int tile_buffer_ok(uint32_t addr, uint32_t len) {
  uint32_t end = addr + len;
  if (end < addr) return 0;
  if (addr >= tile_program_end() && end <= TILE_PROGRAM_END) return 1;
  return addr >= 0x8000u && end <= tile_spm_bytes();
}

// Compute tile index n = y * NX + x, and back
static inline unsigned tile_index(xy_t t) { return t.y * grid_nx() + t.x; }
static inline xy_t tile_at(unsigned n) {
  unsigned nx = grid_nx();
  xy_t r = {(uint8_t)(n % nx), (uint8_t)(n / nx)};
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
// End the program: exit word, wait for both DMA channels, DONE += 1 in the
// I/O tile, console exit (simulation). Returning from kernel_main does this.
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

#define NIU_CMD_DOORBELL (1u << 5)              // ATOMIC_INC: also ring the target's doorbell
#define NIU_CMD_ARG(n)   ((uint32_t)(n) << 16)    // ATOMIC_INC: the increment
// Plane: automatic (fewer hops, ties to NoC0; fixed per source and destination,
// so flits between two tiles keep their order), or forced
#define NIU_PLANE_AUTO   0u
#define NIU_PLANE_NOC0   1u
#define NIU_PLANE_NOC1   2u
#define NIU_CMD_PLANE(p) ((uint32_t)(p) << 3)

// DMA (SPEC.md, Ordering and completion). Addresses and lengths are multiples
// of 16; every call returns at once and the channel stays busy until done.
// dma_write: done when every flit has been injected (not delivered).
void dma_write(int ch, uint32_t src, xy_t dst, uint32_t dst_addr, uint32_t len);
// dma_read (channel 0): len bytes from src_addr at tile src into the local
// scratchpad at dst; done when every reply flit has landed here. The source may
// be the scratchpad, tile control or the counter bank of any compute tile.
void dma_read(xy_t src, uint32_t src_addr, uint32_t dst, uint32_t len);
void dma_wait(int ch);   // until the channel's command has finished
// Semaphores: 32-bit words in the scratchpad. sem_inc adds n (< 65536) at
// dst through channel 1 (waits for it to be idle) and returns once the
// increment is injected; sem_inc_doorbell also sets the target's DOORBELL.
void sem_inc(xy_t dst, uint32_t sem_addr, uint32_t n);
void sem_inc_doorbell(xy_t dst, uint32_t sem_addr, uint32_t n);
// Wait until *sem has reached value (modulo 2^32, so counters may wrap)
static inline void sem_wait(volatile uint32_t *sem, uint32_t value) {
  while ((int32_t)(*sem - value) < 0) {
  }
}
// Every earlier write from this tile to dst is visible at dst when this returns
// (a 16-byte READ_REQ round trip on channel 0). Not for the I/O tile.
void noc_fence(xy_t dst);
// The compute tile dx, dy away on the NX x NY compute torus (wrapping over the
// compute grid only: the I/O row is skipped)
xy_t neighbor(int dx, int dy);
// The plane every dma_*, sem_* and noc_fence call of this program uses
// (NIU_PLANE_*, default automatic). Forcing NoC0 for all traffic keeps the
// ordering rules (one plane per pair still) and measures a one-plane NoC.
void noc_set_plane(uint32_t plane);

// Kernel entry, called by crt0 with the mailbox address.
typedef struct args args_t;
int kernel_main(const args_t *args);

#ifdef __cplusplus
}
#endif
