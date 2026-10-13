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
| Heap usage | growth of the heap end since the last reset, tracked in `_sbrk` |
| Reset | `NVIC_SystemReset()`, once the UART sent its last byte |
| `app_tick` | from `SysTick_Handler`, every millisecond |
| `app_on_fault` | from the HardFault handler, with the faulting `pc` |
| `app_on_exit` | default does nothing: `main()` then parks the CPU |
| Flash/NVM | not yet |

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
  them. `bsp_cli.py` sets the stack (64 KiB, in DTCM) and the heap (64 KiB, in SRAM) with
  `--defsym`.
- `Library/`: the device headers, the StdDriver headers, and the four StdDriver sources the BSP uses
  (clk, sys, uart, gpio), with Nuvoton's licence.
- `CMSIS/`: CMSIS Core headers, with Arm's licence.
- `include/bsp_api.h`: the contract header, identical to lean-bsp's canonical copy (`lbsp check` verifies it).
- `bsp.toml`, `bsp_cli.py`: what lean-bsp reads and runs.
