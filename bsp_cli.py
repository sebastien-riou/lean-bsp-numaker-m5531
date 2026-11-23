#!/usr/bin/env python3
"""lean-bsp entry point of the NuMaker-M5531 BSP, made of lean-bsp's board helpers (leanbsp.backends).

build compiles the device pack's startup code (RTE/), the StdDriver sources it needs (Library/),
src/ and the application with the arm-none-eabi toolchain named by CROSS_COMPILE (default:
arm-none-eabi- from PATH), with the CPU flags of bsp.toml, and links with linker/default.ld (or the
application's own): the pack's linker script RTE/Device/M5531H2LJAE/M5531.ld plus NVM region 0,
STACK_SIZE and HEAP_SIZE set below.
load and run go through lean-hwbridge; the board is the usbman nickname in HWBRIDGE_BOARD, default
numaker-m5531.
"""

import json
import sys
from pathlib import Path

try:
    from leanbsp.backends import board, gcc
except ImportError:
    print(json.dumps({'status': 'error', 'message': 'this BSP needs a lean-bsp with leanbsp.backends: update lean-bsp'}))
    sys.exit(1)

ROOT = Path(__file__).resolve().parent
DEVICE = ROOT / 'RTE' / 'Device' / 'M5531H2LJAE'

SOURCES = [
    DEVICE / 'startup_M5531.c',
    DEVICE / 'system_M5531.c',
    *(ROOT / 'Library' / 'StdDriver' / 'src' / f'{m}.c' for m in ('clk', 'sys', 'uart', 'gpio')),
    *sorted((ROOT / 'src').glob('*.c')),
]
INCLUDE_DIRS = [
    ROOT / 'include',
    ROOT / 'RTE' / '_Debug_M5531H2LJAE',
    DEVICE,
    ROOT / 'Library' / 'Device' / 'Nuvoton' / 'M5531' / 'Include',
    ROOT / 'Library' / 'StdDriver' / 'inc',
    ROOT / 'CMSIS' / 'Core' / 'Include',
]
DEFINES = []
STACK_SIZE = 0x10000  # at the top of DTCM (128 KiB)
HEAP_SIZE = 0x10000   # in SRAM; the pack's 2 KiB default is short once newlib-nano allocates stdout's buffer


def build(app: dict, target, bdir: Path) -> dict:
    return gcc.build(app, target, bdir, root=ROOT, sources=SOURCES, include_dirs=INCLUDE_DIRS, defines=DEFINES,
                     linker_script=ROOT / 'linker' / 'default.ld',  # the pack's M5531.ld plus NVM region 0
                     link_flags=[f'-L{ROOT / "linker"}', f'-L{DEVICE}',
                                 f'-Wl,--defsym=STACK_SIZE={STACK_SIZE:#x}', f'-Wl,--defsym=HEAP_SIZE={HEAP_SIZE:#x}'])


BOARD = board.Board(
    root=ROOT,
    title='NuMaker-M5531',
    flash_by_hand='pyocd load --probe cmsisdap: --cbuild-run <a make.cbuild-run.yml without its output: section> '
                  '{hex}, then power cycle the board',
    run_by_hand='Press the RESET button and read the Nu-Link2-Me VCOM port',
)

if __name__ == '__main__':
    sys.exit(board.main(BOARD, build))
