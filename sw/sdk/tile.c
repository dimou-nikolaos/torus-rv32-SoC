// SDK runtime: console, exit, trap reporting and newlib system calls.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "tile.h"

void dma_write(int ch, uint32_t src, xy_t dst, uint32_t dst_addr, uint32_t len) {
  uint32_t base = NIU_CH(ch);
  mmio_write(base + NIU_SRC, src);
  mmio_write(base + NIU_DST_ADDR, dst_addr);
  mmio_write(base + NIU_DST_XY, (uint32_t)dst.x | (uint32_t)dst.y << 8);
  mmio_write(base + NIU_LEN, len);
  mmio_write(base + NIU_CMD, NIU_OP_WRITE);
}

void dma_read(xy_t src, uint32_t src_addr, uint32_t dst, uint32_t len) {
  uint32_t base = NIU_CH(0);
  mmio_write(base + NIU_SRC, dst);
  mmio_write(base + NIU_DST_ADDR, src_addr);
  mmio_write(base + NIU_DST_XY, (uint32_t)src.x | (uint32_t)src.y << 8);
  mmio_write(base + NIU_LEN, len);
  mmio_write(base + NIU_CMD, NIU_OP_READ_REQ);
}

static void atomic_inc(xy_t dst, uint32_t sem_addr, uint32_t n, uint32_t flags) {
  uint32_t base = NIU_CH(1);
  dma_wait(1);
  mmio_write(base + NIU_DST_ADDR, sem_addr);
  mmio_write(base + NIU_DST_XY, (uint32_t)dst.x | (uint32_t)dst.y << 8);
  mmio_write(base + NIU_CMD, NIU_OP_ATOMIC_INC | flags | NIU_CMD_ARG(n));
  dma_wait(1);
}

void sem_inc(xy_t dst, uint32_t sem_addr, uint32_t n) { atomic_inc(dst, sem_addr, n, 0); }
void sem_inc_doorbell(xy_t dst, uint32_t sem_addr, uint32_t n) { atomic_inc(dst, sem_addr, n, NIU_CMD_DOORBELL); }

void noc_fence(xy_t dst) {
  static uint8_t landing[16] __attribute__((aligned(16)));
  dma_wait(0);
  dma_read(dst, 0, (uint32_t)landing, sizeof landing);
  dma_wait(0);
}

xy_t neighbor(int dx, int dy) {
  xy_t me = my_xy();
  int nx = (int)grid_nx(), ny = (int)grid_ny();
  xy_t r = {(uint8_t)(((me.x + dx) % nx + nx) % nx), (uint8_t)(((me.y + dy) % ny + ny) % ny)};
  return r;
}

void dma_wait(int ch) {
  while (niu_status(ch) & NIU_STATUS_BUSY) {
  }
}

void console_puts(const char *s) {
  while (*s) console_putc(*s++);
}

void tile_exit(int code) {
  mmio_write(TILE_CONSOLE_BASE + CONSOLE_EXIT, (uint32_t)code);
  for (;;) {
  }
}

static void put_hex(uint32_t v) {
  for (int i = 7; i >= 0; i--) console_putc("0123456789abcdef"[(v >> (4 * i)) & 0xf]);
}

// Weak, so a test can install its own handler.
__attribute__((weak)) void tile_trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval) {
  console_puts("\n*** trap: mcause=0x");
  put_hex(mcause);
  console_puts(" mepc=0x");
  put_hex(mepc);
  console_puts(" mtval=0x");
  put_hex(mtval);
  console_puts("\n");
  tile_exit(0x80 | (int)(mcause & 0x7f));
}

// ---------------------------------------------------------------- newlib
extern char __heap_start[], __heap_end[];
static char *heap_ptr = __heap_start;

void *_sbrk(ptrdiff_t incr) {
  char *prev = heap_ptr;
  if (heap_ptr + incr > __heap_end || heap_ptr + incr < __heap_start) {
    errno = ENOMEM;
    return (void *)-1;
  }
  heap_ptr += incr;
  return prev;
}

int _write(int fd, const char *buf, int len) {
  (void)fd;
  for (int i = 0; i < len; i++) console_putc(buf[i]);
  return len;
}

void _exit(int code) { tile_exit(code); }

int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
