/*
 * flash_config.c — Flash parameter storage for STM32U073
 *
 * Uses the last page of 128KB flash (page 63, address 0x0801F800)
 * for storing device configuration.
 * STM32U073 flash page size = 2KB, double-word (64-bit) programming.
 *
 * Layout: each parameter is stored as one 64-bit double-word,
 *         with the value in the lowest byte and 0xFF padding.
 */

#include "flash_config.h"
#include "stm32u0.h"

/* Last 2KB page of 128KB flash */
#define CONFIG_PAGE_ADDR  0x0801F800UL
#define CONFIG_PAGE_NUM   63

static void flash_unlock(void) {
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static void flash_lock(void) {
    FLASH_CR |= FLASH_CR_LOCK;
}

static void flash_wait(void) {
    while (FLASH_SR & FLASH_SR_BSY);
}

void writeFlash(deviceData_t *data) {
    flash_unlock();
    flash_wait();

    /* Erase page */
    FLASH_CR = FLASH_CR_PER | ((uint32_t)CONFIG_PAGE_NUM << 3);
    FLASH_CR |= FLASH_CR_STRT;
    flash_wait();
    FLASH_CR &= ~FLASH_CR_PER;

    /* Clear EOP */
    FLASH_SR = FLASH_SR_EOP;

    /* Write each parameter as a 64-bit double-word */
    FLASH_CR |= FLASH_CR_PG;

    uint8_t *ptr = (uint8_t *)data;
    for (int i = 0; i < (int)sizeof(deviceData_t); i++) {
        volatile uint32_t *addr = (volatile uint32_t *)(CONFIG_PAGE_ADDR + i * 8);
        addr[0] = ptr[i];           /* Low word: value in lowest byte */
        addr[1] = 0xFFFFFFFF;       /* High word: unused */
        flash_wait();
        FLASH_SR = FLASH_SR_EOP;
    }

    FLASH_CR &= ~FLASH_CR_PG;
    flash_lock();
}

void readFlash(addrEnum addr, uint8_t *read_data) {
    volatile uint32_t *flash_addr = (volatile uint32_t *)(CONFIG_PAGE_ADDR + (uint32_t)addr * 8);
    uint32_t val = *flash_addr;
    /* 0xFF = erased/unwritten */
    if (val == 0xFFFFFFFF)
        *read_data = 0xFF;
    else
        *read_data = (uint8_t)(val & 0xFF);
}
