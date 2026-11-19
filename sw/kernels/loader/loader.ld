/* Resident loader (sw/kernels/loader): linked into 0x6800..0x73FF, the region
 * every program leaves alone (sw/sdk/link.ld).
 * 0x0000_6800 - 0x0000_72FF  the resident loader: code, data, stack (sw/kernels/loader)
 * 0x0000_7300 - 0x0000_73FF  its image table and counters (kept across loads)
 * 0x0000_7400 - 0x0000_7EFF  semaphores and kernel sync words
 * 0x0000_7F00 - 0x0000_7FFF  mailbox (runtime args from the host)
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  SPM     (rwx) : ORIGIN = 0x00006800, LENGTH = 0x0B00
  TABLE   (rw)  : ORIGIN = 0x00007300, LENGTH = 0x0100
  SYNC    (rw)  : ORIGIN = 0x00007400, LENGTH = 0x0B00
  MAILBOX (rw)  : ORIGIN = 0x00007F00, LENGTH = 0x0100
}

__stack_size = 512;

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
