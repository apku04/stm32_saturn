/*
 * ota_apply.c — copy a staged OTA image over the running application.
 *
 * The copy routine (ota_copy_and_reset) MUST run from RAM: it erases and
 * reprograms the application flash it would otherwise be executing from. It is
 * placed in the .ramfunc section (see the linker script) which Reset_Handler
 * copies into SRAM during startup. It references only immediates and the FLASH
 * registers — no .rodata, no calls into flash-resident code — so it stays valid
 * after the application region is erased.
 *
 * See ota_apply.h for the power-fail-safety contract.
 */

#include "ota_apply.h"
#include "flash_ota.h"
#include "stm32u0.h"
#include <stdint.h>
#include <stdbool.h>

#define APP_BASE        0x08000000UL
#define APP_PAGE_COUNT  32u            /* pages 0..31 = 64KB application region */

/* Error/EOP flags to clear in FLASH_SR (mirrors flash_ota.c). */
#define OTA_SR_CLEAR \
    (FLASH_SR_EOP | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | \
     (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 14) | (1u << 15))

/* Wait for a just-triggered flash erase/program to complete.
 *
 * MUST be used after setting STRT (erase) or writing the second program word.
 * flash_ota.c gets away with a bare `while (BSY)` because it executes from
 * flash: the instruction fetch immediately after the trigger stalls on the busy
 * bank, which hides the controller's BSY set-up latency. ota_copy_and_reset runs
 * from RAM with no such stall, so a bare `while (BSY)` can be evaluated before
 * BSY asserts, fall straight through, and the following `FLASH_CR &= ~PER/PG`
 * aborts the operation (erases silently do nothing -> later PROGERR on the
 * non-erased page -> apply wedges). The dsb orders the trigger write, and the
 * bounded guard waits for BSY to actually assert before polling for it to clear;
 * the bound prevents an infinite loop if a (fast) op already finished. */
#define OTA_FLASH_WAIT_OP() do {                                          \
    __asm volatile ("dsb 0xF" ::: "memory");                             \
    for (volatile uint32_t _g = 0;                                       \
         _g < 32u && !(FLASH_SR & FLASH_SR_BSY); _g++) { }               \
    while (FLASH_SR & FLASH_SR_BSY) { }                                  \
} while (0)

/* ---- RAM-resident: erase application, copy staged image, system reset ----
 *
 * Never returns: on completion (or unrecoverable error) it issues a system
 * reset. After the reset the new application boots; because the application
 * now matches the staging bank, ota_apply_at_boot() clears the pending flag. */
__attribute__((section(".ramfunc"), noinline, used))
static void ota_copy_and_reset(uint32_t size)
{
    __asm volatile ("cpsid i" ::: "memory");   /* disable interrupts */

    /* Unlock flash */
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
    while (FLASH_SR & FLASH_SR_BSY)
        ;
    FLASH_SR = OTA_SR_CLEAR;

    /* Erase the application pages (0..31). */
    for (uint32_t page = 0; page < APP_PAGE_COUNT; page++) {
        IWDG_KR = IWDG_KR_REFRESH;          /* harmless if watchdog not running */
        while (FLASH_SR & FLASH_SR_BSY)
            ;
        FLASH_CR = FLASH_CR_PER | (page << 3);
        FLASH_CR |= FLASH_CR_STRT;
        OTA_FLASH_WAIT_OP();
        FLASH_CR &= ~FLASH_CR_PER;
        FLASH_SR = FLASH_SR_EOP;
    }

    /* Programming requires a clean control register and cleared error flags,
     * otherwise the first PG operation faults with PGSERR and every write is
     * rejected (this was the original brick: the app was erased but nothing was
     * ever written back). Mirror flash_ota.c's ota_write(): clear FLASH_CR's
     * leftover erase bits (PER/STRT/PNB) and clear FLASH_SR before programming. */
    FLASH_CR = 0;
    FLASH_SR = OTA_SR_CLEAR;

    /* Program staged image (OTA bank) into the application region, 8 bytes at
     * a time. Round the byte count up to a double-word boundary. */
    uint32_t dwords = (size + 7u) / 8u;
    volatile uint32_t *src = (volatile uint32_t *)OTA_BANK_BASE;
    volatile uint32_t *dst = (volatile uint32_t *)APP_BASE;

    for (uint32_t i = 0; i < dwords; i++) {
        if ((i & 0x3Fu) == 0)
            IWDG_KR = IWDG_KR_REFRESH;

        uint32_t lo = src[i * 2];
        uint32_t hi = src[i * 2 + 1];

        /* Retry each double-word up to 3 times on write error. */
        uint32_t attempt;
        for (attempt = 0; attempt < 3u; attempt++) {
            while (FLASH_SR & FLASH_SR_BSY)
                ;
            FLASH_SR = OTA_SR_CLEAR;
            FLASH_CR |= FLASH_CR_PG;
            dst[i * 2]     = lo;
            dst[i * 2 + 1] = hi;
            OTA_FLASH_WAIT_OP();
            FLASH_SR = FLASH_SR_EOP;
            FLASH_CR &= ~FLASH_CR_PG;
            /* Readback verify — if it matches we're done with this word */
            if (dst[i * 2] == lo && dst[i * 2 + 1] == hi)
                break;
            /* Mismatch: clear PG and CR fully before retry */
            FLASH_CR = 0;
            FLASH_SR = OTA_SR_CLEAR;
        }
        /* If all retries failed: lock and reset so ota_apply_at_boot retries
         * the whole transfer on next boot rather than clearing the pending flag. */
        if (attempt == 3u) {
            FLASH_CR |= FLASH_CR_LOCK;
            SCB_AIRCR = 0x05FA0004u;
            while (1) ;
        }
    }

    FLASH_CR |= FLASH_CR_LOCK;

    /* System reset — boot into the freshly written application. */
    SCB_AIRCR = 0x05FA0004u;   /* VECTKEY | SYSRESETREQ */
    while (1)
        ;
}

/* ---- Self-test (flash-resident; never touches the application) ----
 *
 * Reproduces the applier's erase + program-from-flash + verify sequence on the
 * last OTA-bank page (page 62, well beyond any real image), using the start of
 * the application as a known source. Lets the flash sequence be validated on a
 * live node with zero brick risk. Returns true if the page reads back matching
 * the source. */
#define OTA_SCRATCH_ADDR  0x0801F000UL   /* page 62: last OTA-bank page */
#define OTA_SCRATCH_PAGE  62u
#define OTA_SCRATCH_DWORDS 256u          /* 2KB / 8 */

bool ota_self_test(void)
{
    volatile uint32_t *scratch = (volatile uint32_t *)OTA_SCRATCH_ADDR;
    volatile uint32_t *source  = (volatile uint32_t *)APP_BASE;

    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
    while (FLASH_SR & FLASH_SR_BSY)
        ;
    FLASH_SR = OTA_SR_CLEAR;

    /* erase scratch page */
    FLASH_CR = FLASH_CR_PER | (OTA_SCRATCH_PAGE << 3);
    FLASH_CR |= FLASH_CR_STRT;
    while (FLASH_SR & FLASH_SR_BSY)
        ;
    FLASH_CR &= ~FLASH_CR_PER;
    FLASH_SR = FLASH_SR_EOP;

    /* program — identical sequence to ota_copy_and_reset */
    FLASH_CR = 0;
    FLASH_SR = OTA_SR_CLEAR;
    for (uint32_t i = 0; i < OTA_SCRATCH_DWORDS; i++) {
        uint32_t lo = source[i * 2];
        uint32_t hi = source[i * 2 + 1];
        while (FLASH_SR & FLASH_SR_BSY)
            ;
        FLASH_SR = OTA_SR_CLEAR;
        FLASH_CR |= FLASH_CR_PG;
        scratch[i * 2]     = lo;
        scratch[i * 2 + 1] = hi;
        while (FLASH_SR & FLASH_SR_BSY)
            ;
        FLASH_SR = FLASH_SR_EOP;
        FLASH_CR &= ~FLASH_CR_PG;
    }
    FLASH_CR |= FLASH_CR_LOCK;

    for (uint32_t i = 0; i < OTA_SCRATCH_DWORDS * 2; i++)
        if (scratch[i] != source[i])
            return false;
    return true;
}

/* ---- Boot-time entry (runs from flash; safe — no erase here) ---- */

/* True if the application region differs from the staged image over `size`
 * bytes. A plain byte compare; the image CRC was already verified by the
 * receiver before the pending flag was set. */
static bool app_differs_from_bank(uint32_t size)
{
    const volatile uint8_t *a = (const volatile uint8_t *)APP_BASE;
    const volatile uint8_t *b = (const volatile uint8_t *)OTA_BANK_BASE;
    for (uint32_t i = 0; i < size; i++) {
        if ((i & 0x0FFFu) == 0)
            IWDG_KR = IWDG_KR_REFRESH;   /* harmless if watchdog not running */
        if (a[i] != b[i])
            return true;
    }
    return false;
}

void ota_apply_at_boot(void)
{
    uint16_t size = 0;
    if (!ota_is_pending(&size))
        return;

    if (size == 0 || (uint32_t)size > ota_bank_size()) {
        /* Corrupt/implausible pending record — discard it. */
        ota_clear_pending();
        return;
    }

    if (app_differs_from_bank(size)) {
        ota_copy_and_reset(size);   /* runs from RAM; resets, does not return */
        return;                     /* unreachable */
    }

    /* Application already matches the staged image: the update is complete.
     * Now running from stable flash, so it is safe to clear the flag. */
    ota_clear_pending();
}
