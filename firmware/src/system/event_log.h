/*
 * event_log.h — persistent boot/reset event log on W25Q32 ext flash
 *
 * One 32-byte record is appended per boot, in a ring buffer that occupies
 * the first 4 KB sector of the external flash. Survives power-loss, USB/
 * battery hand-over, and BOR / IWDG resets — the failure modes that .noinit
 * RAM cannot survive.
 *
 * Layout (32 bytes per record, 128 records per 4 KB sector):
 *   off  size  field
 *    0    2    magic   = 0xBEEF      (0xFFFF means slot is empty)
 *    2    2    flags   bit0 = reached_main_loop on previous boot
 *    4    4    boot_count   (diag.boot_count at the moment of write)
 *    8    4    rst_csr      (raw RCC_CSR snapshot)
 *   12    4    last_uptime_ms (last beacon-saved uptime from prev boot)
 *   16    1    last_init_stage (highest stage from prev boot)
 *   17    1    this_init_stage (init_stage when log was written, currently 9..)
 *   18   14    reserved (0xFF, available for CRC/extension)
 *
 * The host can decode rst_csr exactly the same way as the live beacon v7
 * field, so log decoding reuses existing tooling.
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdint.h>

#define EVENT_LOG_REC_SIZE   32
#define EVENT_LOG_REC_COUNT  128
#define EVENT_LOG_MAGIC      0xBEEFu

typedef struct {
    uint16_t magic;
    uint16_t flags;
    uint32_t boot_count;
    uint32_t rst_csr;
    uint32_t last_uptime_ms;
    uint8_t  last_init_stage;
    uint8_t  this_init_stage;
    uint8_t  reserved[14];
} __attribute__((packed)) event_log_rec_t;

_Static_assert(sizeof(event_log_rec_t) == EVENT_LOG_REC_SIZE,
               "event_log_rec_t must be 32 bytes");

/* Scan flash, find the next free slot (and the count of valid records).
 * Safe to call when ext_flash is missing — becomes a no-op. */
void event_log_init(void);

/* Append one record. If the sector is full, erase + restart from slot 0.
 * No-op if ext_flash isn't present. */
void event_log_append(uint32_t boot_count,
                      uint32_t rst_csr,
                      uint32_t last_uptime_ms,
                      uint8_t  last_init_stage,
                      uint8_t  this_init_stage,
                      uint8_t  reached_main);

/* Read record by absolute slot index (0..EVENT_LOG_REC_COUNT-1).
 * Returns 1 if the slot has a valid record, 0 otherwise. */
int  event_log_read(uint32_t slot, event_log_rec_t *out);

/* Number of valid records currently in the sector. */
uint32_t event_log_count(void);

/* Index of the next slot that will be written (head). */
uint32_t event_log_head(void);

/* Erase the entire log sector (drops all history). */
void event_log_clear(void);

#endif /* EVENT_LOG_H */
