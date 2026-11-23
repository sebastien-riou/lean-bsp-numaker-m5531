/* NVM region 0: APROM bank 1 of the M5531's 2 MB flash (0x00200000, 1 MB), which the image never
 * occupies (linker/default.ld checks it). The code runs from bank 0, which the controller keeps
 * readable while it erases or programs bank 1.
 *
 * Straight to the flash memory controller (FMC), the sequences of the device pack's StdDriver
 * fmc.c (FMC_Erase, FMC_Write, FMC_Write8Bytes) without their time-outs: like the other BSPs', an
 * operation is waited for until the controller is done. Each operation runs with interrupts masked
 * and the protected registers unlocked, ISP and APROM update enabled for that operation only.
 *  - erase: page by page (8 KiB), then each page must read all 0xFF: the controller's fail flag
 *    covers what it could not do (a disabled update, an illegal address, a brown-out...), not the
 *    result;
 *  - write: every destination byte must read erased first, or nothing is programmed and the write
 *    returns BSP_NVM_REFUSED: the reference manual neither allows nor refuses programming a
 *    programmed word, hence no zero_overwrite, and the controller does not check the target, so
 *    this check is the only source of BSP_NVM_REFUSED. Then one program per write unit, in
 *    ascending order: a 32-bit program for a 4-byte unit, a 64-bit one for an 8-byte unit (so that
 *    a unit's value and its padding are programmed by the same operation). Then read back;
 *  - read: memcpy.
 * The flash is cacheable: each erase and program invalidates the data cache over what it changed.
 * Status: BSP_NVM_OK; 1, 2 outside the region; 3 misaligned address; 4 size not a multiple of the
 * program unit; 5 a page or a write read back different; 7 the controller's fail flag (ISPFF) after
 * a command; BSP_NVM_REFUSED a destination byte was not erased, nothing programmed. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "NuMicro.h"
#include "bsp_api.h"
#include "bsp_nvm.h"

#define NVM0_BASE       FMC_APROM_BANK1_BASE
#define NVM0_SIZE       FMC_APROM_BANK_SIZE
#define PAGE_SIZE       FMC_FLASH_PAGE_SIZE
#define NVM0_WRITE_SIZE BSP_NVM0_WRITE_SIZE /* bsp_nvm.h */

_Static_assert(NVM0_WRITE_SIZE == 4u || NVM0_WRITE_SIZE == 8u, "the FMC programs 32 or 64 bits at once");

static inline bool in_region(uintptr_t addr, uintptr_t size) {
    return addr >= NVM0_BASE && size <= NVM0_SIZE && addr - NVM0_BASE <= NVM0_SIZE - size;
}

static bool erased(const uint8_t *p, uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++) {
        if (p[i] != 0xFFu) return false;
    }
    return true;
}

#define ISP_FAILED 7u /* the controller's fail flag */

/* One ISP command, interrupts masked; true if the controller raised its fail flag. The flag is
 * cleared first, so that a stale one cannot fail this command. */
static bool isp(uint32_t cmd, uint32_t addr, const uint32_t *data) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    SYS_UnlockReg();
    FMC->ISPCTL |= FMC_ISPCTL_ISPEN_Msk | FMC_ISPCTL_APUEN_Msk;
    FMC->ISPCTL |= FMC_ISPCTL_ISPFF_Msk; /* write 1 to clear */
    FMC->ISPCMD = cmd;
    FMC->ISPADDR = addr;
    if (cmd == FMC_ISPCMD_PROGRAM) {
        FMC->ISPDAT = data[0];
    } else if (cmd == FMC_ISPCMD_PROGRAM_64) {
        FMC->MPDAT0 = data[0];
        FMC->MPDAT1 = data[1];
    }
    FMC->ISPTRG = FMC_ISPTRG_ISPGO_Msk;
    while (FMC->ISPTRG & FMC_ISPTRG_ISPGO_Msk) {
    }
    while (FMC->ISPSTS & FMC_ISPSTS_ISPBUSY_Msk) {
    }
    const bool failed = (FMC->ISPCTL & FMC_ISPCTL_ISPFF_Msk) || (FMC->ISPSTS & FMC_ISPSTS_ISPFF_Msk);
    if (failed) FMC->ISPCTL |= FMC_ISPCTL_ISPFF_Msk; /* write 1 to clear */
    FMC->ISPCTL &= ~(FMC_ISPCTL_APUEN_Msk | FMC_ISPCTL_ISPEN_Msk);
    SYS_LockReg();
    __set_PRIMASK(primask);
    return failed;
}

static uint8_t nvm0_erase(void *base_address, unsigned n_pages) {
    if (n_pages == 0) return BSP_NVM_OK;
    const uintptr_t base = (uintptr_t)base_address;
    if ((uintptr_t)n_pages > NVM0_SIZE / PAGE_SIZE) return 2;
    if (!in_region(base, (uintptr_t)n_pages * PAGE_SIZE)) return base < NVM0_BASE ? 1 : 2;
    if (base % PAGE_SIZE) return 3;
    for (unsigned i = 0; i < n_pages; i++) {
        const uintptr_t page = base + (uintptr_t)i * PAGE_SIZE;
        const bool failed = isp(FMC_ISPCMD_PAGE_ERASE, (uint32_t)page, NULL);
        SCB_InvalidateDCache_by_Addr((void *)page, (int32_t)PAGE_SIZE);
        if (failed) return ISP_FAILED;
        if (!erased((const uint8_t *)page, PAGE_SIZE)) return 5;
    }
    return BSP_NVM_OK;
}

static uint8_t nvm0_write(void *dst, const void *src, uintptr_t size) {
    if (size == 0) return BSP_NVM_OK;
    const uintptr_t addr = (uintptr_t)dst;
    if (!in_region(addr, size)) return addr < NVM0_BASE ? 1 : 2;
    if (addr % NVM0_WRITE_SIZE) return 3;
    if (size % NVM0_WRITE_SIZE) return 4;
    if (!erased(dst, size)) return BSP_NVM_REFUSED;
    const uint8_t *from = src;
    const uint32_t cmd = NVM0_WRITE_SIZE == 8u ? FMC_ISPCMD_PROGRAM_64 : FMC_ISPCMD_PROGRAM;
    for (uintptr_t at = addr; at < addr + size; at += NVM0_WRITE_SIZE, from += NVM0_WRITE_SIZE) {
        uint32_t unit[2];
        memcpy(unit, from, NVM0_WRITE_SIZE); /* the source may be misaligned */
        const bool failed = isp(cmd, (uint32_t)at, unit);
        SCB_InvalidateDCache_by_Addr((void *)at, (int32_t)NVM0_WRITE_SIZE);
        if (failed) return ISP_FAILED;
    }
    return memcmp(dst, src, size) ? 5 : BSP_NVM_OK;
}

static uint8_t nvm0_read(void *dst, const void *src, uintptr_t size) {
    if (!in_region((uintptr_t)src, size)) return (uintptr_t)src < NVM0_BASE ? 1 : 2;
    memcpy(dst, src, size);
    return BSP_NVM_OK;
}

const bsp_nvm_region_t bsp_nvm_regions[BSP_N_NVM_REGIONS] = {
    {
        .name = "aprom-bank1",
        .base_address = (void *)NVM0_BASE,
        .size = NVM0_SIZE,
        .page_size = PAGE_SIZE,
        .write_size = NVM0_WRITE_SIZE,
        .zero_overwrite = false, /* not documented for the M5531 */
        .erase = nvm0_erase,
        .write = nvm0_write,
        .read = nvm0_read,
    },
};
