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
