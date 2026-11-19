/* Tile memory map (SPEC.md, Tile memory map and NIU registers), the same at
 * every scratchpad size (32 or 64 KB):
 * 0x0000_0000 - 0x0000_67FF  program region: code, data, .bss, then the stack
 *                            (boot at 0x0); free above __program_end (heap,
 *                            buffers the host places)
 * 0x0000_6800 - 0x0000_73FF  reserved: the resident loader (sw/kernels/loader)
 * 0x0000_7400 - 0x0000_7EFF  semaphores and kernel sync words
 * 0x0000_7F00 - 0x0000_7FFF  mailbox (runtime args from the host)
 * 0x0000_8000 - 0x0000_FFFF  64 KB scratchpads only: buffers
 */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  SPM     (rwx) : ORIGIN = 0x00000000, LENGTH = 0x6800
  SYNC    (rw)  : ORIGIN = 0x00007400, LENGTH = 0x0B00
  MAILBOX (rw)  : ORIGIN = 0x00007F00, LENGTH = 0x0100
}

__stack_size = DEFINED(__stack_size) ? __stack_size : 2K;

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

  /* The stack right above .bss, so the program's footprint is [0,
     __program_end) and buffers can start right above it; the heap takes the
     rest of the region (host-placed buffers and malloc do not mix) */
  __stack_top   = ALIGN(16) + __stack_size;
  __program_end = __stack_top;
  __heap_start  = __stack_top;
  __heap_end    = ORIGIN(SPM) + LENGTH(SPM);
  ASSERT(__program_end <= __heap_end, "program and stack do not fit in the program region")

  .sync (NOLOAD) : { *(.sync .sync.*) } > SYNC
  .mailbox (NOLOAD) : { __mailbox = .; *(.mailbox) } > MAILBOX

  /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
