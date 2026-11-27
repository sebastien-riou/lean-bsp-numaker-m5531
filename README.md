# lean-bsp-numaker-m5531

The [lean-bsp](https://github.com/sebastien-riou/lean-bsp) BSP for the Nuvoton NuMaker-M5531 board
(M5531H2LJAE, Cortex-M55 at 220 MHz). Any lean-bsp application builds and runs on it:

````
lbsp build-load-run --bsp numaker-m5531 --until 'hello-world done' --timeout 60
````

## What it needs

- The arm-none-eabi toolchain. `CROSS_COMPILE` selects it, e.g.
  `CROSS_COMPILE=/opt/toolchains/arm-none-eabi-gcc/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin/arm-none-eabi-`;
  without it, `arm-none-eabi-gcc` is taken from `PATH`.
- For `load` and `run`, [lean-hwbridge](https://github.com/sebastien-riou/lean-hwbridge) serving the
  board with its `pyocd` profile: `hwbridge` on `PATH`, or the command in `HWBRIDGE`. The board is
  its usbman nickname, `HWBRIDGE_BOARD`, by default `numaker-m5531`. Without a bridge, `load` prints
  the pyocd command to flash the board by hand.

## How the contract maps to the board

| Primitive | On NuMaker-M5531 |
|---|---|
| UART 0 (and `printf`, `stdin`) | UART0 (PB12 RX, PB13 TX), the Nu-Link2-Me's VCOM, 115200 8N1. Reception is polled from the 16-byte FIFO. |
| LEDs | 0 red (PH4), 1 yellow (PD6), 2 green (PD5) |
| Buttons | 0 is BTN_0 (PI11), 1 is BTN_1 (PH1) |
| Cycle counter, CPU frequency | DWT `CYCCNT`, at `SystemCoreClock`; see below for what debuggers do to it |
| Delay | busy wait on SysTick's counter: works with interrupts masked, and whatever a debugger did to the DWT |
| Cache clear | the core's L1 caches: D-cache cleaned and invalidated, I-cache invalidated |
| Heap usage | lean-bsp's heap counter (contract 1.4), `src/bsp_heap.c`: the bytes requested from `malloc`, `calloc` and `realloc` and not freed yet, at most since the last reset. The heap itself is newlib's, grown by `_sbrk` from the end of the data to the end of the SRAM region (0x20230000) |
| Reset | `NVIC_SystemReset()`, once the UART sent its last byte |
| `app_tick` | from `SysTick_Handler`, every millisecond |
| `app_on_fault` | from the HardFault handler, with the faulting `pc` |
| `app_on_exit` | default does nothing: `main()` then parks the CPU |
| Flash/NVM | region 0, `aprom-bank1`: APROM bank 1 (0x00200000, 1 MiB), 8 KiB pages, 8-byte write unit (one 64-bit program per unit), no zero overwrite (the reference manual neither allows nor refuses programming a programmed word). The image must fit bank 0, the lower 1 MiB (the link checks it). Through the FMC's registers, without the pack driver's time-outs: a write refuses a destination that does not read erased, programs the units in ascending order and reads back; an erase verifies its pages read erased; each command clears the controller's fail flag first and reports it as its own status; the data cache is invalidated over what each command changed |

### The cycle counter and the debugger

The DWT lives in the debug power domain, which debuggers switch on and off under the firmware
(found with crypto-benchmark-m5531):

- pyocd writes `DEMCR = 0` when it disconnects from a running core (`pyocd reset`, a debug session
  ending), which stops the counter under the firmware it just started. lean-hwbridge's `reset`
  avoids that: it halts the core under reset, then lets it go with `pyocd commander -c go`, whose
  disconnect leaves `DEMCR` alone. For the other cases, `bsp_cycle_count()` sets `TRCENA` again
  whenever it finds it cleared, and so does the SysTick handler every millisecond, so a measurement
  loses at most a millisecond while interrupts are enabled.
- Loading through the device pack runs its `DebugPortStop` sequence, which powers the debug domain
  down: the counter then stays frozen until a debugger powers it up. lean-hwbridge's `flash` ends
  with its `reset`, which does (pyocd's generic `cortex_m` target loads no such sequence).

## Layout

There is no generated project: `src/bsp_numaker_m5531.c` brings the board up as
[crypto-benchmark-m5531](https://github.com/sebastien-riou/crypto-benchmark-m5531) does and holds
`main()`, the primitives and the default application hooks. The rest is Nuvoton's and Arm's, copied
unchanged from the device pack crypto-benchmark-m5531 uses (NuMicroM55_DFP 3.1.4-rc.3, CMSIS 6.3.0):

- `RTE/`: the startup code, `system_M5531.c` and the linker script `M5531.ld`, as csolution placed
  them. The build uses a copy of `M5531.ld` (below) rather than this one.
- `linker/M5531-lean-bsp.ld`: the pack's `M5531.ld` with one change, the stack moved from the top of
  DTCM to the start of SRAM, below the data, as crypto-benchmark-m5531 has it (`diff` against the
  pack's shows the whole change), and the check that the stack stays in SRAM0, which the start-up code
  uses before it powers SRAM1 and SRAM2 on. `bsp_cli.py` sets the stack (256 KiB, where MSPLIM stops
  it) and the heap's reserve (64 KiB) with `--defsym`; the heap then grows to the end of the SRAM region.
- `linker/default.ld`: `M5531-lean-bsp.ld` plus NVM region 0 (`NVM0`, where the sections named
  `.bsp_nvm0` go, loading nothing there) and the check that the image stays below it, and the heap's
  limit (`__lean_bsp_heap_limit`, the end of the SRAM region). An application needing its own script
  ends it with `INCLUDE default.ld`, as on every board.
- `src/bsp_nvm.c`: NVM region 0, on the FMC's registers (`Library/.../fmc_reg.h`, `fmc.h`).
- `Library/`: the device headers, the StdDriver headers, and the four StdDriver sources the BSP uses
  (clk, sys, uart, gpio), with Nuvoton's licence.
- `CMSIS/`: CMSIS Core headers, with Arm's licence.
- `include/bsp_api.h`: the contract header, identical to lean-bsp's canonical copy (`lbsp check` verifies it).
- `bsp.toml`, `bsp_cli.py`: what lean-bsp reads and runs. `bsp_cli.py` is this board's data on top of lean-bsp's
  board helpers (`leanbsp.backends`), which lean-bsp runs it with.
