/*
 * ext_flash.h — W25Q32 external SPI flash driver (bit-bang on PB5/PB10/PB11/PB12)
 *
 * 4 MB (32 Mbit) NOR flash, 256-byte pages, 4 KB sectors, 64 KB blocks.
 */

#ifndef EXT_FLASH_H
#define EXT_FLASH_H

#include <stdint.h>

#define EXT_FLASH_PAGE_SIZE    256
#define EXT_FLASH_SECTOR_SIZE  4096
#define EXT_FLASH_BLOCK_SIZE   65536
#define EXT_FLASH_TOTAL_SIZE   (4UL * 1024 * 1024)  /* 4 MB */

void     ext_flash_init(void);

/* True after ext_flash_init() succeeded (JEDEC ID matched and BSY cleared
 * within the init timeout). False means the chip is missing/dead and other
 * ext_flash_* calls should be skipped. */
int      ext_flash_present(void);

/* Read JEDEC manufacturer / device ID (3 bytes).  Returns 0xEF4016 for W25Q32. */
uint32_t ext_flash_read_id(void);

/* Read `len` bytes starting at `addr` into `buf`.  No alignment requirements. */
void     ext_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/*
 * Write `len` bytes from `buf` into flash starting at `addr`.
 * Handles page boundaries automatically.
 * The target region MUST be erased first (all 0xFF).
 */
void     ext_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len);

/* Erase a 4 KB sector containing `addr`.  Blocks until complete. */
void     ext_flash_erase_sector(uint32_t addr);

/* Erase a 64 KB block containing `addr`.  Blocks until complete. */
void     ext_flash_erase_block(uint32_t addr);

/* Erase entire chip.  Blocks until complete (can take several seconds). */
void     ext_flash_erase_chip(void);

#endif /* EXT_FLASH_H */
