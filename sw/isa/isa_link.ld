/* ISA tests: code and data in the scratchpad, entry at 0x0 */
OUTPUT_ARCH("riscv")
ENTRY(_start)
SECTIONS
{
  . = 0x0;
  .text.init : { *(.text.init) }
  .text : { *(.text) }
  . = ALIGN(0x100);
  .data : { *(.data) }
  .bss : { *(.bss) }
  ASSERT(. <= 0xF000, "ISA test does not fit")
}
