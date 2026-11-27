/* The default linker script, under the name lean-bsp's other boards give theirs, so that an application's
 * own script can end with `INCLUDE default.ld` on every board. Here it is the device pack's
 * RTE/Device/M5531H2LJAE/M5531.ld, unchanged, plus NVM region 0: bsp_cli.py puts this directory and the
 * pack's on the linker's search path. */
INCLUDE M5531.ld

/* NVM region 0: APROM bank 1, the upper 1 MB of the flash (src/bsp_nvm.c). The application's data there is
 * laid out at link time, in the sections named .bsp_nvm0; nothing is loaded there. */
MEMORY
{
  NVM0 (rw) : ORIGIN = 0x00200000, LENGTH = 0x00100000
}

SECTIONS
{
  .bsp_nvm0 (NOLOAD) :
  {
    KEEP(*(.bsp_nvm0))
    KEEP(*(.bsp_nvm0.*))
  } > NVM0
}

/* The heap: newlib's _sbrk (src/bsp_numaker_m5531.c) grows it up to the end of the SRAM region, past the
 * HEAP_SIZE that .heap reserves (bsp_cli.py), .heap being the region's last section. */
__lean_bsp_heap_limit = ORIGIN(SRAM) + LENGTH(SRAM);
ASSERT(__HeapLimit <= __lean_bsp_heap_limit, "the heap's reserve does not fit the SRAM region")

/* The stack, at the top of DTCM (STACK_SIZE, bsp_cli.py), must leave the DTCM data below it alone: MSPLIM
 * stops it at __StackLimit, so an overlap would only show as corrupted data. */
ASSERT(__dtcm_zi_end__ <= __StackLimit, "the stack (STACK_SIZE) overlaps the data placed in DTCM")

/* __esram_data: the end of what the image loads into flash (its code, then the ITCM and data images) */
ASSERT(__esram_data <= ORIGIN(NVM0), "the image reaches into NVM region 0: it must fit APROM bank 0, the lower 1 MB of flash")
