/* Resident loader (sw/kernels/loader): linked into 0xE000..0xEFFF, the region
 * every program leaves alone (sw/sdk/link.ld).
 * 0x0000_E000 - 0x0000_EEFF  the resident loader: code, data, stack (sw/kernels/loader)
 * 0x0000_EF00 - 0x0000_EFFF  its image table and counters (kept across loads)
 * 0x0000_F000 - 0x0000_FEFF  semaphores and kernel sync words
 * 0x0000_FF00 - 0x0000_FFFF  mailbox (runtime args from the host)
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  SPM     (rwx) : ORIGIN = 0x0000E000, LENGTH = 0x0F00
  TABLE   (rw)  : ORIGIN = 0x0000EF00, LENGTH = 0x0100
  SYNC    (rw)  : ORIGIN = 0x0000F000, LENGTH = 0x0F00
  MAILBOX (rw)  : ORIGIN = 0x0000FF00, LENGTH = 0x0100
}

__stack_size = 1K;

SECTIONS
{
  .text : {
    KEEP(*(.text.init))
    *(.text .text.*)
  } > SPM

  .rodata : ALIGN(16) {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  } > SPM

  .data : ALIGN(16) {
    *(.data .data.*)
    __global_pointer$ = . + 0x800;
    *(.sdata .sdata.*)
  } > SPM

  .bss (NOLOAD) : ALIGN(16) {
    __bss_start = .;
    *(.sbss .sbss.* .bss .bss.* COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > SPM

  /* Heap from the end of .bss up to the stack */
  __heap_start = ALIGN(16);
  __stack_top  = ORIGIN(SPM) + LENGTH(SPM);
  __heap_end   = __stack_top - __stack_size;
  ASSERT(__heap_start <= __heap_end, "program does not fit below the stack")

  .table (NOLOAD) : { *(.table) } > TABLE
  .sync (NOLOAD) : { *(.sync .sync.*) } > SYNC
  .mailbox (NOLOAD) : { __mailbox = .; *(.mailbox) } > MAILBOX

  /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
