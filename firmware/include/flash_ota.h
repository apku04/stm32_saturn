/*
 * flash_ota.h — OTA firmware bank management for STM32U073
 *
 * Layout (128KB flash):
 *   0x08000000 – 0x0800FFFF  pages  0-31  64KB  Application (running)
 *   0x08010000 – 0x0801EFFF  pages 32-62  62KB  OTA firmware bank
 *   0x0801F800 – 0x0801FFFF  page  63      2KB  Device config
 */

#ifndef FLASH_OTA_H
#define FLASH_OTA_H

#include <stdint.h>
#include <stdbool.h>

/* OTA bank geometry */
#define OTA_BANK_BASE       0x08010000UL
#define OTA_BANK_PAGE_START 32
#define OTA_BANK_PAGE_END   62          /* inclusive */
#define OTA_BANK_PAGES      (OTA_BANK_PAGE_END - OTA_BANK_PAGE_START + 1) /* 31 */
#define OTA_BANK_SIZE       (OTA_BANK_PAGES * 2048UL)                     /* 63488 bytes */

typedef enum {
    OTA_OK        =  0,
    OTA_ERR_SIZE  = -1,   /* offset + len exceeds bank */
    OTA_ERR_ALIGN = -2,   /* write not 8-byte aligned */
    OTA_ERR_ERASE = -3,   /* erase verify failed */
    OTA_ERR_WRITE = -4,   /* write readback mismatch */
} ota_err_t;

/* Bank info */
uint32_t  ota_bank_addr(void);
uint32_t  ota_bank_size(void);

/* Erase entire OTA bank (31 pages) */
ota_err_t ota_erase(void);

/* Write len bytes into OTA bank at byte offset.
 * offset and len MUST be multiples of 8. */
ota_err_t ota_write(uint32_t offset, const uint8_t *data, uint32_t len);

/* Read len bytes from OTA bank at byte offset (no alignment requirement). */
void      ota_read(uint32_t offset, uint8_t *buf, uint32_t len);

/* Pending flag — stored in config page slots */
void      ota_set_pending(uint16_t image_size);
void      ota_clear_pending(void);
bool      ota_is_pending(uint16_t *image_size);

#endif /* FLASH_OTA_H */
