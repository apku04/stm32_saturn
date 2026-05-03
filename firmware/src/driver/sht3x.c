/*
 * sht3x.c — Sensirion SHT3x driver (single-shot, clock-stretching disabled)
 *
 * Datasheet ref: SHT3x-DIS rev 7, §4.3 (single shot) and §4.13 (conversion).
 * Command 0x2400 = single shot, high repeatability, no clock stretching.
 * Conversion takes <= 15 ms; we poll the I2C ACK to know when data is ready.
 */

#include "sht3x.h"
#include "bb_i2c.h"

#define CMD_SOFT_RESET_HI  0x30
#define CMD_SOFT_RESET_LO  0xA2
#define CMD_SS_HIGH_HI     0x24   /* single-shot, no clock-stretch, high rep */
#define CMD_SS_HIGH_LO     0x00

static uint8_t  s_addr      = 0x44;
static uint8_t  s_present   = 0;
static int16_t  s_temp_cdeg = 0;
static uint16_t s_hum_cpct  = 0;

/* Sensirion CRC-8: poly 0x31, init 0xFF (datasheet §4.12). */
static uint8_t crc8(const uint8_t *d, uint8_t len)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

static int send_cmd(uint8_t hi, uint8_t lo)
{
    uint8_t buf[2] = { hi, lo };
    return bb_i2c_write(s_addr, buf, 2);
}

static int probe(uint8_t a)
{
    uint8_t dummy;
    if (bb_i2c_read(a, &dummy, 1) == 0) return 1;
    return 0;
}

int sht3x_init(void)
{
    s_present = 0;
    bb_i2c_init();

    if      (probe(0x44)) s_addr = 0x44;
    else if (probe(0x45)) s_addr = 0x45;
    else return -1;

    /* Soft reset */
    send_cmd(CMD_SOFT_RESET_HI, CMD_SOFT_RESET_LO);
    for (volatile int i = 0; i < 200000; i++) __asm__("nop");   /* >1 ms */

    s_present = 1;
    return 0;
}

int sht3x_present(void) { return s_present; }

int sht3x_sample(void)
{
    if (!s_present) return -1;

    if (send_cmd(CMD_SS_HIGH_HI, CMD_SS_HIGH_LO) < 0) return -1;

    /* Conversion ≤15 ms in high-rep mode. With clock-stretching disabled
     * the sensor NACKs reads until done — poll up to ~30 ms. */
    uint8_t raw[6];
    int ok = -1;
    for (int i = 0; i < 60; i++) {
        for (volatile int d = 0; d < 10000; d++) __asm__("nop");
        if (bb_i2c_read(s_addr, raw, 6) == 0) { ok = 0; break; }
    }
    if (ok < 0) return -1;

    if (crc8(&raw[0], 2) != raw[2]) return -2;
    if (crc8(&raw[3], 2) != raw[5]) return -2;

    uint16_t raw_t = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t raw_h = ((uint16_t)raw[3] << 8) | raw[4];

    /* T_cdeg = -4500 + (17500 * raw_t) / 65535
     * H_cpct =          (10000 * raw_h) / 65535
     * Use 32-bit math, round half-up. */
    int32_t t = (int32_t)(((uint32_t)raw_t * 17500u + 32767u) / 65535u) - 4500;
    int32_t h =  (int32_t)((uint32_t)raw_h * 10000u + 32767u) / 65535u;
    if (h < 0) h = 0;
    if (h > 10000) h = 10000;

    s_temp_cdeg = (int16_t)t;
    s_hum_cpct  = (uint16_t)h;
    return 0;
}

int16_t  sht3x_get_temp_cdeg(void) { return s_temp_cdeg; }
uint16_t sht3x_get_hum_cpct(void)  { return s_hum_cpct;  }
