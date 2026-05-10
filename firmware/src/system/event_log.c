/*
 * event_log.c — persistent boot/reset event log on W25Q32
 *
 * See event_log.h for the on-flash record layout and rationale.
 *
 * Concurrency: caller is single-threaded (everything runs in the main
 * loop / ISRs that don't touch flash). No locking.
 */

#include "event_log.h"
#include "../driver/ext_flash.h"
#include <string.h>

/* The log lives in the first 4 KB sector of the ext flash. We don't share
 * this sector with anything else — keep it dedicated so a future feature
 * adding more data nearby can't accidentally erase the history. */
#define LOG_SECTOR_ADDR    0x000000u
#define LOG_SECTOR_SIZE    4096u

static uint32_t s_head  = 0;   /* next slot to write (0..REC_COUNT) */
static uint32_t s_count = 0;   /* number of valid records currently stored */

/* Read just the magic word of one slot. Cheap probe used during scan. */
static uint16_t read_magic(uint32_t slot)
{
    uint16_t m = 0xFFFFu;
    ext_flash_read(LOG_SECTOR_ADDR + slot * EVENT_LOG_REC_SIZE,
                   (uint8_t *)&m, sizeof(m));
    return m;
}

void event_log_init(void)
{
    s_head  = 0;
    s_count = 0;

    if (!ext_flash_present())
        return;

    /* Records are appended in slot order, so the first slot whose magic is
     * 0xFFFF (erased flash) marks the next free position. Anything before
     * it counts as a valid record. If every slot is full, the sector is
     * full and the next append will erase + start over. */
    for (uint32_t i = 0; i < EVENT_LOG_REC_COUNT; i++) {
        uint16_t m = read_magic(i);
        if (m == 0xFFFFu) {
            s_head  = i;
            s_count = i;
            return;
        }
        if (m != EVENT_LOG_MAGIC) {
            /* Corrupt slot (partial write before reset?) — treat it as
             * end-of-log and let append() rotate the sector when needed. */
            s_head  = i;
            s_count = i;
            return;
        }
    }

    /* Fully populated sector. */
    s_head  = EVENT_LOG_REC_COUNT;
    s_count = EVENT_LOG_REC_COUNT;
}

void event_log_append(uint32_t boot_count,
                      uint32_t rst_csr,
                      uint32_t last_uptime_ms,
                      uint8_t  last_init_stage,
                      uint8_t  this_init_stage,
                      uint8_t  reached_main)
{
    if (!ext_flash_present())
        return;

    /* If the sector is full, erase and restart. We lose history but the
     * device keeps logging — preferable to silently dropping events. A
     * future improvement is to use 2 sectors and ping-pong. */
    if (s_head >= EVENT_LOG_REC_COUNT) {
        ext_flash_erase_sector(LOG_SECTOR_ADDR);
        s_head  = 0;
        s_count = 0;
    }

    event_log_rec_t rec;
    memset(&rec, 0xFF, sizeof(rec));   /* leave reserved bytes erased */
    rec.magic           = EVENT_LOG_MAGIC;
    rec.flags           = (uint16_t)(reached_main ? 0x0001 : 0x0000);
    rec.boot_count      = boot_count;
    rec.rst_csr         = rst_csr;
    rec.last_uptime_ms  = last_uptime_ms;
    rec.last_init_stage = last_init_stage;
    rec.this_init_stage = this_init_stage;

    ext_flash_write(LOG_SECTOR_ADDR + s_head * EVENT_LOG_REC_SIZE,
                    (const uint8_t *)&rec, sizeof(rec));

    s_head++;
    s_count++;
}

int event_log_read(uint32_t slot, event_log_rec_t *out)
{
    if (!ext_flash_present() || slot >= EVENT_LOG_REC_COUNT || !out)
        return 0;
    ext_flash_read(LOG_SECTOR_ADDR + slot * EVENT_LOG_REC_SIZE,
                   (uint8_t *)out, sizeof(*out));
    return out->magic == EVENT_LOG_MAGIC;
}

uint32_t event_log_count(void) { return s_count; }
uint32_t event_log_head(void)  { return s_head; }

void event_log_clear(void)
{
    if (!ext_flash_present())
        return;
    ext_flash_erase_sector(LOG_SECTOR_ADDR);
    s_head  = 0;
    s_count = 0;
}
