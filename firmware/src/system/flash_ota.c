/*
 * flash_ota.c — OTA firmware bank management for STM32U073
 *
 * Bare-metal flash erase / program / read for the OTA bank
 * (pages 32-62, 0x08010000 – 0x0801EFFF).
 *
 * OTA pending flag and image size are stored in the device-config page
 * (page 63) using the addrEnum slot mechanism from flash_config.c.
 */

#include "flash_ota.h"
#include "globalInclude.h"
#include "stm32u0.h"
#include "flash_config.h"

#define FLASH_SR_CLEAR_FLAGS \
    (FLASH_SR_EOP | (1u << 1) | (1u << 3) | (1u << 4) | \
     (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | \
     (1u << 9) | (1u << 14) | (1u << 15))

/* ---- Local flash helpers (duplicated from flash_config.c) ---- */

static void flash_unlock(void)
{
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static void flash_lock(void)
{
    FLASH_CR |= FLASH_CR_LOCK;
}

static void flash_wait(void)
{
    while (FLASH_SR & FLASH_SR_BSY)
        ;
}

static void flash_clear_status(void)
{
    FLASH_SR = FLASH_SR_CLEAR_FLAGS;
}

/* ---- Public: bank info ---- */

uint32_t ota_bank_addr(void) { return OTA_BANK_BASE; }
uint32_t ota_bank_size(void) { return OTA_BANK_SIZE; }

/* ---- Public: erase ---- */

ota_err_t ota_erase(void)
{
    flash_unlock();
    flash_clear_status();

    for (uint32_t page = OTA_BANK_PAGE_START; page <= OTA_BANK_PAGE_END; page++) {
        flash_wait();
        FLASH_CR = FLASH_CR_PER | (page << 3);
        FLASH_CR |= FLASH_CR_STRT;
        flash_wait();
        FLASH_CR &= ~FLASH_CR_PER;
        FLASH_SR = FLASH_SR_EOP;
    }

    flash_lock();

    /* Verify first and last words are erased */
    volatile uint32_t *base = (volatile uint32_t *)OTA_BANK_BASE;
    volatile uint32_t *last = (volatile uint32_t *)(OTA_BANK_BASE + OTA_BANK_SIZE - 4);
    if (*base != 0xFFFFFFFF || *last != 0xFFFFFFFF)
        return OTA_ERR_ERASE;

    return OTA_OK;
}

/* ---- Public: write ---- */

ota_err_t ota_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if ((offset & 7) || (len & 7))
        return OTA_ERR_ALIGN;

    if (offset + len > OTA_BANK_SIZE)
        return OTA_ERR_SIZE;

    flash_unlock();
    flash_clear_status();

    uint32_t addr_base = OTA_BANK_BASE + offset;
    const uint32_t *src = (const uint32_t *)data;
    uint32_t dwords = len / 8;

    for (uint32_t i = 0; i < dwords; i++) {
        volatile uint32_t *dst =
            (volatile uint32_t *)(addr_base + i * 8);

        uint32_t lo = src[i * 2];
        uint32_t hi = src[i * 2 + 1];

        flash_wait();
        FLASH_CR |= FLASH_CR_PG;
        dst[0] = lo;
        dst[1] = hi;
        flash_wait();
        FLASH_SR = FLASH_SR_EOP;
        FLASH_CR &= ~FLASH_CR_PG;

        /* Readback verify */
        if (dst[0] != lo || dst[1] != hi) {
            flash_lock();
            return OTA_ERR_WRITE;
        }
    }

    flash_lock();
    return OTA_OK;
}

/* ---- Public: read ---- */

void ota_read(uint32_t offset, uint8_t *buf, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)(OTA_BANK_BASE + offset);
    for (uint32_t i = 0; i < len; i++)
        buf[i] = src[i];
}

/* ---- Public: pending flag ----
 *
 * Stored in the device-config page (page 63) via flash_config's
 * writeFlashParamByte/readFlash. We deliberately reuse that API rather than a
 * parallel writer: writeFlashParamByte rewrites *all* config slots on every
 * call (erased slots become the double-word 0x000000FF), so a "write only if
 * still 0xFFFFFFFF" approach silently fails after any other parameter has been
 * persisted. The read-modify-erase-rewrite path always succeeds. */

void ota_set_pending(uint16_t image_size)
{
    writeFlashParamByte(OTA_IMAGE_SIZE_LO, (uint8_t)(image_size & 0xFF));
    writeFlashParamByte(OTA_IMAGE_SIZE_HI, (uint8_t)((image_size >> 8) & 0xFF));
    writeFlashParamByte(OTA_PENDING_FLAG,  0x01);   /* set last: commit point */
}

void ota_clear_pending(void)
{
    writeFlashParamByte(OTA_PENDING_FLAG, 0x00);
}

bool ota_is_pending(uint16_t *image_size)
{
    uint8_t flag = 0xFF;
    readFlash(OTA_PENDING_FLAG, &flag);
    if (flag != 0x01)
        return false;

    if (image_size) {
        uint8_t lo = 0, hi = 0;
        readFlash(OTA_IMAGE_SIZE_LO, &lo);
        readFlash(OTA_IMAGE_SIZE_HI, &hi);
        *image_size = (uint16_t)lo | ((uint16_t)hi << 8);
    }
    return true;
}
