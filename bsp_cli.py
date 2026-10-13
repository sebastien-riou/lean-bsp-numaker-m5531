#!/usr/bin/env python3
"""lean-bsp entry point of the NuMaker-M5531 BSP: capabilities, build, load and run.

build compiles the device pack's startup code (RTE/), the StdDriver sources it needs (Library/),
src/ and the application with the arm-none-eabi toolchain named by CROSS_COMPILE (default:
arm-none-eabi- from PATH), with the CPU flags of bsp.toml, and links with the pack's linker script
RTE/Device/M5531H2LJAE/M5531.ld (or the application's own), STACK_SIZE and HEAP_SIZE set below.

load and run go through lean-hwbridge (`hwbridge`, or the command in HWBRIDGE), so they work from
a sandboxed session as well as from a terminal. The board is the usbman nickname in HWBRIDGE_BOARD,
default numaker-m5531. Without a bridge, load reports what to do by hand.

Every verb ends its output with one JSON line, the result lean-bsp parses.
"""

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tomllib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STATE = 'numaker-m5531.json'  # this BSP's record of what it built and where the boot output starts

DEVICE = ROOT / 'RTE' / 'Device' / 'M5531H2LJAE'
BOARD_SOURCES = [
    DEVICE / 'startup_M5531.c',
    DEVICE / 'system_M5531.c',
    *(ROOT / 'Library' / 'StdDriver' / 'src' / f'{m}.c' for m in ('clk', 'sys', 'uart', 'gpio')),
    *sorted((ROOT / 'src').glob('*.c')),
]
BOARD_INCLUDES = [
    ROOT / 'include',
    ROOT / 'RTE' / '_Debug_M5531H2LJAE',
    DEVICE,
    ROOT / 'Library' / 'Device' / 'Nuvoton' / 'M5531' / 'Include',
    ROOT / 'Library' / 'StdDriver' / 'inc',
    ROOT / 'CMSIS' / 'Core' / 'Include',
]
BOARD_DEFINES = []
STACK_SIZE = 0x10000  # at the top of DTCM (128 KiB)
HEAP_SIZE = 0x10000   # in SRAM; the pack's 2 KiB default is short once newlib-nano allocates stdout's buffer


def result(status: str, **fields) -> int:
    print(json.dumps({'status': status, **fields}), flush=True)
    return 1 if status == 'error' else 0


def target_cflags(name: str) -> list[str]:
    with open(ROOT / 'bsp.toml', 'rb') as f:
        manifest = tomllib.load(f)
    for target in manifest['bsp']['targets']:
        if target['name'] == name:
            return list(target['cflags'])
    raise SystemExit(f'no target {name!r} in bsp.toml')


def read_state(bdir: Path) -> dict:
    path = bdir / STATE
    return json.loads(path.read_text()) if path.is_file() else {}


def write_state(bdir: Path, **fields) -> None:
    (bdir / STATE).write_text(json.dumps({**read_state(bdir), **fields}) + '\n')


# ---- build ----


def cmd_capabilities(args) -> int:
    return result('ok')


def cmd_build(args) -> int:
    app = json.loads(Path(args.app_json).read_text())
    bdir = Path(args.bdir)
    obj_dir = bdir / 'obj'
    obj_dir.mkdir(parents=True, exist_ok=True)
    prefix = os.environ.get('CROSS_COMPILE', 'arm-none-eabi-')
    cc = f'{prefix}gcc'
    cpu = target_cflags(args.target)
    common = [*cpu, '-Og', '-g', '-Wall', '-fdata-sections', '-ffunction-sections']
    board = [*common, *(f'-D{d}' for d in BOARD_DEFINES), *(f'-I{d}' for d in BOARD_INCLUDES)]
    application = [*board, *(f'-I{d}' for d in app['include_dirs']), *(f'-D{d}' for d in app['defines'])]
    units = [(src, board) for src in BOARD_SOURCES] + [(Path(src), application) for src in app['sources']]
    if app.get('lib'):
        return result('error', message='prebuilt application libraries ([app.build]) are not supported yet')

    def compile_one(unit):
        src, flags = unit
        obj = obj_dir / f'{src.stem}-{hashlib.sha1(str(src).encode()).hexdigest()[:8]}.o'  # same name in two dirs
        language = ['-x', 'assembler-with-cpp'] if src.suffix == '.s' else []
        cmd = [cc, *language, *flags, '-c', str(src), '-o', str(obj)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        return src, obj, cmd, done

    objects = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for src, obj, cmd, done in pool.map(compile_one, units):
            print(shlex.join(cmd) if src in map(Path, app['sources']) else f'cc {src.relative_to(ROOT)}', flush=True)
            sys.stdout.write(done.stdout + done.stderr)
            if done.returncode != 0:
                return result('error', message=f'compiling {src} failed')
            objects.append(str(obj))
    script = Path(app['linker_script']) if app.get('linker_script') else DEVICE / 'M5531.ld'
    elf = bdir / f'{app["name"]}.elf'
    link = [cc, *cpu, '-specs=nano.specs', f'-T{script}', f'-Wl,--defsym=STACK_SIZE={STACK_SIZE:#x}',
            f'-Wl,--defsym=HEAP_SIZE={HEAP_SIZE:#x}', *objects,
            '-Wl,--gc-sections', f'-Wl,-Map={bdir / (app["name"] + ".map")},--cref', '-lc', '-lm', '-lnosys',
            '-o', str(elf)]
    print(shlex.join(link), flush=True)
    if subprocess.run(link).returncode != 0:
        return result('error', message='linking failed')
    hex_file, bin_file = elf.with_suffix('.hex'), elf.with_suffix('.bin')
    for fmt, out in (('ihex', hex_file), ('binary', bin_file)):
        if subprocess.run([f'{prefix}objcopy', '-O', fmt, str(elf), str(out)]).returncode != 0:
            return result('error', message=f'objcopy -O {fmt} failed')
    subprocess.run([f'{prefix}size', str(elf)])
    write_state(bdir, elf=str(elf), hex=str(hex_file))
    return result('ok', artifact={'elf': str(elf), 'bin': str(bin_file), 'hex': str(hex_file)})


# ---- load and run, through lean-hwbridge ----


def board() -> str:
    return os.environ.get('HWBRIDGE_BOARD', 'numaker-m5531')


def hwbridge(*args: str, capture: bool = True) -> subprocess.CompletedProcess:
    cmd = [*shlex.split(os.environ.get('HWBRIDGE', 'hwbridge')), *args]
    print(shlex.join(cmd), flush=True)
    return subprocess.run(cmd, capture_output=capture, text=True)


class NoBridge(Exception):
    pass


def bridge_json(*args: str) -> dict:
    try:
        done = hwbridge(*args, '--json')
    except FileNotFoundError:
        raise NoBridge('hwbridge is not installed') from None
    if done.returncode == 3:
        raise NoBridge(done.stderr.strip())
    if done.returncode != 0:
        raise RuntimeError(done.stderr.strip() or f'hwbridge {args[0]} failed ({done.returncode})')
    return json.loads(done.stdout) if done.stdout.strip() else {}


def cmd_load(args) -> int:
    bdir = Path(args.bdir)
    state = read_state(bdir)
    if 'elf' not in state:
        return result('error', message=f'nothing built in {bdir}')
    try:
        bridge_json('serial', 'open', board())  # first, so that the boot output is captured
        flashed = bridge_json('flash', board(), state['elf'])
    except NoBridge as e:
        return result('manual', message=f'{e}. Flash it yourself with pyocd and the M5531 device pack, e.g. pyocd '
                                         f'load --probe cmsisdap: --cbuild-run <a make.cbuild-run.yml without its '
                                         f'output: section> {state["hex"]}, then power cycle the board; or start '
                                         f'`hwbridge serve`')
    except RuntimeError as e:
        return result('error', message=str(e))
    write_state(bdir, boot=[flashed['generation'], flashed['offset']])
    return result('ok', boot=f'{flashed["generation"]}:{flashed["offset"]}')


def cmd_run(args) -> int:
    com_device = f'hwbridge:{board()}'
    try:
        bridge_json('serial', 'open', board())
        reset = bridge_json('reset', board())
    except NoBridge as e:
        return result('manual', message=f'{e}. Press the RESET button and read the Nu-Link2-Me VCOM port')
    except RuntimeError as e:
        return result('error', message=str(e))
    boot = f'{reset["generation"]}:{reset["offset"]}'
    if args.detach:
        return result('ok', com_device=com_device, exit_code=None, boot=boot)
    # follow the console: a board has no exit code, so this lasts until --until, --timeout or Ctrl-C
    cat = ['serial', 'cat', board(), '--from', boot]
    if args.until:
        cat += ['--until', args.until]
    if args.timeout is not None and args.timeout >= 0:
        cat += ['--timeout', str(args.timeout)]
    done = hwbridge(*cat, capture=False)
    print(flush=True)
    if done.returncode == 124 and args.until:
        return result('error', message=f'{args.until!r} did not show up within {args.timeout:g} s',
                      com_device=com_device, exit_code=None)
    if done.returncode not in (0, 124):
        return result('error', message=f'following the console failed ({done.returncode})',
                      com_device=com_device, exit_code=None)
    return result('ok', com_device=com_device, exit_code=None, boot=boot)


def main() -> int:
    parser = argparse.ArgumentParser(prog='bsp_cli.py', description='NuMaker-M5531 BSP entry point for lean-bsp')
    sub = parser.add_subparsers(dest='verb', required=True)
    p = sub.add_parser('capabilities')
    p.add_argument('--json', action='store_true')
    p.set_defaults(func=cmd_capabilities)
    p = sub.add_parser('build')
    p.add_argument('--app-json', required=True)
    p.add_argument('--target', required=True)
    p.add_argument('--bdir', required=True)
    p.set_defaults(func=cmd_build)
    p = sub.add_parser('load')
    p.add_argument('--bdir', required=True)
    p.add_argument('--target')
    p.set_defaults(func=cmd_load)
    p = sub.add_parser('run')
    p.add_argument('--bdir', required=True)
    p.add_argument('--target')
    p.add_argument('--detach', action='store_true')
    p.add_argument('--timeout', type=float, help='seconds to follow the console; -1 or absent: until Ctrl-C')
    p.add_argument('--until', help='stop following once this text was printed')
    p.set_defaults(func=cmd_run)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
