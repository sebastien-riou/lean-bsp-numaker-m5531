/* NuMaker-M5531: the lean-bsp contract on top of Nuvoton's M5531 device pack (StdDriver).
 *
 * There is no vendor-generated main() here: this file brings the board up as
 * crypto-benchmark-m5531 does (220 MHz from APLL0, UART0 on the Nu-Link2-Me's VCOM), then runs the
 * application. The startup code (RTE/Device/M5531H2LJAE) and its linker script are the pack's.
 *
 * Two things a debugger does to this board shape the time primitives:
 *  - pyocd clears DEMCR.TRCENA when it disconnects, about a second after the reset it started us
 *    with, which stops the DWT under the running firmware: bsp_cycle_count() turns it back on;
 *  - loading through the device pack powers the debug domain down (its DebugPortStop sequence),
 *    and the DWT with it, until a debugger powers it up again: lean-hwbridge's reset does. So
 *    bsp_delay_ms() relies on SysTick, which is outside that domain, never on the DWT. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "NuMicro.h"
#include "bsp_api.h"
#include "bsp_nvm.h"

#define CPU_FREQ_HZ FREQ_220MHZ

static const bsp_capabilities_t capabilities = {
    .board_name = "numaker-m5531",
    .n_uarts = 1,
    .n_leds = 3,
    .n_buttons = 2,
    .has_cycle_counter = true,
    .n_nvm_regions = BSP_N_NVM_REGIONS,
    .nvm_regions = bsp_nvm_regions,
};

const bsp_capabilities_t *bsp_get_capabilities(void) { return &capabilities; }

/* ---- the application hooks' defaults ---- */

__attribute__((weak)) void app_init(void) {}

__attribute__((weak)) void app_tick(uint32_t tick_ms) { (void)tick_ms; }

__attribute__((weak)) void app_on_fault(uint32_t reason, uintptr_t pc) {
    (void)reason;
    (void)pc;
}

__attribute__((weak)) void app_on_exit(int code) { (void)code; }  /* main() then parks the CPU */

/* ---- UART 0: UART0 (PB12 RX, PB13 TX), the Nu-Link2-Me's VCOM; stdin and stdout too ---- */

void bsp_uart_tx(unsigned port, const void *buf, size_t len) {
    if (port != 0) return;
    const uint8_t *p = buf;
    while (len--) {
        while (UART0->FIFOSTS & UART_FIFOSTS_TXFULL_Msk) {
        }
        UART0->DAT = *p++;
    }
}

/* Polled, from the 16-byte receive FIFO: what arrives beyond that while nobody calls this is lost. */
size_t bsp_uart_rx(unsigned port, void *buf, size_t len) {
    if (port != 0) return 0;
    uint8_t *p = buf;
    size_t n = 0;
    while (n < len && !(UART0->FIFOSTS & UART_FIFOSTS_RXEMPTY_Msk)) p[n++] = (uint8_t)UART0->DAT;
    return n;
}

/* Until the last byte left the shift register: a reset would otherwise cut it off. */
static void uart_drain(void) {
    while (!(UART0->FIFOSTS & UART_FIFOSTS_TXEMPTYF_Msk)) {
    }
}

/* newlib's stdout and stdin, overriding libnosys */
int _write(int file, char *ptr, int len) {
    (void)file;
    if (len > 0) bsp_uart_tx(0, ptr, (size_t)len);
    return len;
}

int _read(int file, char *ptr, int len) {
    (void)file;
    if (len <= 0) return 0;
    size_t n;
    while ((n = bsp_uart_rx(0, ptr, (size_t)len)) == 0) {
    }
    return (int)n;
}

/* What the STM32 projects' syscalls.c provides: stdout is a terminal, so newlib buffers it by
 * line, not by kilobyte (libnosys says it is not one). */
int _isatty(int file) {
    (void)file;
    return 1;
}

int _fstat(int file, struct stat *st) {
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _close(int file) {
    (void)file;
    return -1;
}

int _lseek(int file, int ptr, int dir) {
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

/* ---- LEDs (0 red, 1 yellow, 2 green) and buttons (0 BTN_0, 1 BTN_1), all active low ---- */

void bsp_led_set(unsigned id, bool on) {
    const uint32_t level = on ? 0 : 1;
    switch (id) {
        case 0: PH4 = level; break;
        case 1: PD6 = level; break;
        case 2: PD5 = level; break;
        default: break;
    }
}

bool bsp_button_get(unsigned id) {
    switch (id) {
        case 0: return PI11 == 0;
        case 1: return PH1 == 0;
        default: return false;
    }
}

/* ---- time ---- */

/* The DWT is locked behind the optional software lock (LAR, which CMSIS does not model for the
 * M55; writing it is harmless where it is not implemented) and DEMCR.TRCENA. */
#define DWT_LAR (*(volatile uint32_t *)(DWT_BASE + 0xFB0UL))

static void dwt_enable(void) {
    DWT_LAR = 0xC5ACCE55UL;
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t bsp_cycle_count(void) {
    if (!(DCB->DEMCR & DCB_DEMCR_TRCENA_Msk)) dwt_enable();  /* a debugger detaching cleared it */
    return DWT->CYCCNT;
}

uint32_t bsp_cpu_freq_hz(void) { return SystemCoreClock; }

static volatile uint32_t tick_ms;

void SysTick_Handler(void) {
    /* A detaching debugger may clear DEMCR mid-measurement: lose at most a millisecond of cycles
     * then, as long as interrupts are enabled. */
    if (!(DCB->DEMCR & DCB_DEMCR_TRCENA_Msk)) dwt_enable();
    tick_ms++;
    app_tick(tick_ms);
}

/* On SysTick's down-counter, polled: works with interrupts masked, and whatever the debugger did
 * to the DWT. */
void bsp_delay_ms(uint32_t ms) {
    const uint32_t period = SysTick->LOAD + 1u;  /* cycles per millisecond */
    uint64_t remaining = (uint64_t)ms * period;
    uint32_t last = SysTick->VAL;
    while (remaining) {
        const uint32_t now = SysTick->VAL;
        const uint32_t elapsed = now <= last ? last - now : last + period - now;
        last = now;
        remaining = elapsed >= remaining ? 0 : remaining - elapsed;
    }
}

void bsp_reset(void) {
    uart_drain();
    NVIC_SystemReset();
}

/* The core's L1 caches: the D-cache is cleaned first, so no write is lost. */
void bsp_clear_caches(void) {
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
}

/* ---- the heap: newlib's, grown here; its use is counted by src/bsp_heap.c (contract 1.4) ---- */

extern char end;                    /* the heap's start, from the device pack's linker script */
extern char __lean_bsp_heap_limit;  /* the end of the SRAM region, from linker/default.ld */
static char *heap_end;

/* The heap grows from the end of the data up to the end of the SRAM region: .heap, which reserves
 * HEAP_SIZE there (bsp_cli.py), is the region's last section. */
void *_sbrk(int incr) {
    if (heap_end == NULL) heap_end = &end;
    char *const prev = heap_end;
    if (incr > &__lean_bsp_heap_limit - heap_end) return (void *)-1;
    heap_end += incr;
    return prev;
}

/* ---- faults ----
 * The pack's HardFault_Handler finds the stacked frame and hands it to ProcessHardFault, then
 * returns, which would re-run the faulting instruction forever: stop here instead. */

uint32_t ProcessHardFault(uint32_t *frame) {
    app_on_fault(APP_FAULT_HARD, frame[6]);
    for (;;) {
    }
}

/* ---- bring-up, as crypto-benchmark-m5531 does it ---- */

static void clocks_init(void) {
    CLK_EnableXtalRC(CLK_SRCCTL_HIRCEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HIRCSTB_Msk);
    CLK_EnableXtalRC(CLK_SRCCTL_HXTEN_Msk);
    CLK_WaitClockReady(CLK_STATUS_HXTSTB_Msk);
    CLK_EnableAPLL(CLK_APLLCTL_APLLSRC_HIRC, CPU_FREQ_HZ, CLK_APLL0_SELECT);
    CLK_SetSCLK(CLK_SCLKSEL_SCLKSEL_APLL0);
    CLK_SET_HCLK2DIV(2);
    CLK_SET_PCLK0DIV(2);
    CLK_SET_PCLK1DIV(2);
    CLK_SET_PCLK2DIV(2);
    CLK_SET_PCLK3DIV(2);
    CLK_SET_PCLK4DIV(4);
    SystemCoreClockUpdate();
}

static void uart0_init(void) {
    CLK_EnableModuleClock(UART0_MODULE);
    SetDebugUartCLK();
    /* Each port has its own clock gate: without GPIOB's, PB12's input path stays dead and only
     * transmitting works. */
    CLK_EnableModuleClock(GPIOB_MODULE);
    SetDebugUartMFP();
    PB->PUSEL = (PB->PUSEL & ~GPIO_PUSEL_PUSEL12_Msk) | (GPIO_PUSEL_PULL_UP << GPIO_PUSEL_PUSEL12_Pos);  /* RX idles high */
    InitDebugUart();
    /* Drop what the line clocked in while the pins were being muxed, and the sticky errors. */
    UART0->FIFO |= UART_FIFO_RXRST_Msk;
    while (UART0->FIFO & UART_FIFO_RXRST_Msk) {
    }
    UART0->FIFOSTS = UART_FIFOSTS_RXOVIF_Msk | UART_FIFOSTS_BIF_Msk | UART_FIFOSTS_FEF_Msk | UART_FIFOSTS_PEF_Msk;
}

static void gpio_init(void) {
    CLK_EnableModuleClock(GPIOD_MODULE);
    CLK_EnableModuleClock(GPIOH_MODULE);
    CLK_EnableModuleClock(GPIOI_MODULE);
    PH4 = 1;  /* LEDs off */
    PD6 = 1;
    PD5 = 1;
    GPIO_SetMode(PD, BIT5 | BIT6, GPIO_MODE_OUTPUT);
    GPIO_SetMode(PH, BIT4, GPIO_MODE_OUTPUT);
    GPIO_SetMode(PH, BIT1, GPIO_MODE_INPUT);
    GPIO_SetMode(PI, BIT11, GPIO_MODE_INPUT);
}

int main(void) {
    SYS_UnlockReg();
    clocks_init();
    uart0_init();
    gpio_init();
    SYS_LockReg();

    __disable_irq();
    dwt_enable();
    bsp_heap_usage_reset();
    SysTick_Config(SystemCoreClock / 1000u);  /* app_tick, and bsp_delay_ms's time base */
    app_init();                                /* IRQs still masked, deterministic ordering */
    __enable_irq();
    app_on_exit(app_main());
    for (;;) {
        /* app_main returned: park here rather than starting over (lean-bsp contract) */
    }
}
