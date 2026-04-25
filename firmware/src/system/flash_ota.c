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

/* Config page address (matches flash_config.c) */
#define CONFIG_PAGE_ADDR  0x0801F800UL
#define CONFIG_PAGE_NUM   63

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

/* ---- Config-page slot helpers (1 byte per 8-byte double-word) ---- */

static uint8_t config_slot_read(addrEnum slot)
{
    volatile uint32_t *addr =
        (volatile uint32_t *)(CONFIG_PAGE_ADDR + (uint32_t)slot * 8);
    uint32_t val = addr[0];
    return (val == 0xFFFFFFFF) ? 0xFF : (uint8_t)(val & 0xFF);
}

static void config_slot_write(addrEnum slot, uint8_t value)
{
    volatile uint32_t *addr =
        (volatile uint32_t *)(CONFIG_PAGE_ADDR + (uint32_t)slot * 8);

    flash_unlock();
    flash_wait();

    FLASH_CR |= FLASH_CR_PG;
    addr[0] = (uint32_t)value;
    addr[1] = 0xFFFFFFFF;
    flash_wait();
    FLASH_SR = FLASH_SR_EOP;
    FLASH_CR &= ~FLASH_CR_PG;

    flash_lock();
}

/* ---- Public: bank info ---- */

uint32_t ota_bank_addr(void) { return OTA_BANK_BASE; }
uint32_t ota_bank_size(void) { return OTA_BANK_SIZE; }

/* ---- Public: erase ---- */

ota_err_t ota_erase(void)
{
    flash_unlock();

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

/* ---- Public: pending flag ---- */

void ota_set_pending(uint16_t image_size)
{
    /* Write only works on erased (0xFF) slots, so writes are safe
     * as long as we erase the config page first when needed.
     * For simplicity we write the slots directly — the config page
     * must already have these slots erased (0xFF).  If a prior
     * pending flag exists, caller should erase config first via
     * a full writeFlash cycle.  In practice the OTA coordinator
     * will do: readFlash → clear existing → writeFlash → set_pending. */
    config_slot_write(OTA_PENDING_FLAG,  0x01);
    config_slot_write(OTA_IMAGE_SIZE_LO, (uint8_t)(image_size & 0xFF));
    config_slot_write(OTA_IMAGE_SIZE_HI, (uint8_t)((image_size >> 8) & 0xFF));
}

void ota_clear_pending(void)
{
    /* Clearing requires erasing the entire config page and rewriting
     * all other parameters.  We read current config, wipe, rewrite. */
    uint8_t params[FLASH_SIZE_PARAM];
    for (int i = 0; i < (int)FLASH_SIZE_PARAM; i++)
        params[i] = config_slot_read((addrEnum)i);

    flash_unlock();
    flash_wait();

    /* Erase config page */
    FLASH_CR = FLASH_CR_PER | ((uint32_t)CONFIG_PAGE_NUM << 3);
    FLASH_CR |= FLASH_CR_STRT;
    flash_wait();
    FLASH_CR &= ~FLASH_CR_PER;
    FLASH_SR = FLASH_SR_EOP;

    /* Rewrite only the original parameters (OTA slots stay erased = 0xFF) */
    FLASH_CR |= FLASH_CR_PG;
    for (int i = 0; i < (int)FLASH_SIZE_PARAM; i++) {
        if (params[i] == 0xFF)
            continue; /* skip erased slots */
        volatile uint32_t *addr =
            (volatile uint32_t *)(CONFIG_PAGE_ADDR + (uint32_t)i * 8);
        addr[0] = (uint32_t)params[i];
        addr[1] = 0xFFFFFFFF;
        flash_wait();
        FLASH_SR = FLASH_SR_EOP;
    }
    FLASH_CR &= ~FLASH_CR_PG;

    flash_lock();
}

bool ota_is_pending(uint16_t *image_size)
{
    uint8_t flag = config_slot_read(OTA_PENDING_FLAG);
    if (flag != 0x01)
        return false;

    if (image_size) {
        uint8_t lo = config_slot_read(OTA_IMAGE_SIZE_LO);
        uint8_t hi = config_slot_read(OTA_IMAGE_SIZE_HI);
        *image_size = (uint16_t)lo | ((uint16_t)hi << 8);
    }
    return true;
}
