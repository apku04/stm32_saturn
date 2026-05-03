/*
 * bme280.c — Bosch BME280 driver (I2C, forced mode, temp + humidity)
 *
 * Reference: Bosch BME280 datasheet (rev 1.6).
 *
 * Compensation formulas are the integer "double-precision-equivalent" path
 * from §4.2.3 / appendix 8.2 of the datasheet, simplified for fixed-point
 * output (centi-°C, centi-%RH).  Pressure is intentionally not computed —
 * the LoRa beacon only ships temperature and humidity.
 */

#include "bme280.h"
#include "bb_i2c.h"

/* ---- Registers ---- */
#define BME_REG_ID          0xD0
#define BME_REG_RESET       0xE0
#define BME_REG_CTRL_HUM    0xF2
#define BME_REG_STATUS      0xF3
#define BME_REG_CTRL_MEAS   0xF4
#define BME_REG_CONFIG      0xF5
#define BME_REG_PRESS_MSB   0xF7   /* press(3) temp(3) hum(2) = 8 bytes */

#define BME_REG_CALIB_T_P   0x88   /* 26 bytes: T1..T3, P1..P9 */
#define BME_REG_CALIB_H1    0xA1   /* 1 byte   */
#define BME_REG_CALIB_H2    0xE1   /* 7 bytes  : H2..H6 packed */

#define BME_CHIP_ID         0x60   /* BMP280 = 0x58, BME280 = 0x60 */

/* ctrl_meas: osrs_t<<5 | osrs_p<<2 | mode
 *   osrs=001 (×1), mode=01 (forced) → 0x25 (T x1, P x1, forced)
 * ctrl_hum: osrs_h x1 = 0x01
 * config:  filter off, standby don't-care → 0x00
 */
#define BME_CTRL_HUM_X1     0x01
#define BME_CTRL_MEAS_FORCE (0x20 | 0x04 | 0x01)   /* T x1, P x1, FORCED */
#define BME_CONFIG_DEFAULT  0x00

/* ---- Calibration data (per-chip) ---- */
static struct {
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint8_t  dig_H1, dig_H3;
    int16_t  dig_H2, dig_H4, dig_H5;
    int8_t   dig_H6;
    int32_t  t_fine;
} cal;

static uint8_t  s_addr      = 0x76;
static uint8_t  s_present   = 0;
static int16_t  s_temp_cdeg = 0;
static uint16_t s_hum_cpct  = 0;

static int read_calibration(void)
{
    uint8_t b1[26];
    uint8_t b2[7];
    uint8_t h1;

    if (bb_i2c_write_then_read(s_addr, BME_REG_CALIB_T_P, b1, 26) < 0) return -1;
    if (bb_i2c_write_then_read(s_addr, BME_REG_CALIB_H1, &h1, 1) < 0) return -1;
    if (bb_i2c_write_then_read(s_addr, BME_REG_CALIB_H2, b2, 7) < 0)  return -1;

    cal.dig_T1 = (uint16_t)(b1[0] | (b1[1] << 8));
    cal.dig_T2 = (int16_t) (b1[2] | (b1[3] << 8));
    cal.dig_T3 = (int16_t) (b1[4] | (b1[5] << 8));

    cal.dig_H1 = h1;
    cal.dig_H2 = (int16_t)(b2[0] | (b2[1] << 8));
    cal.dig_H3 = b2[2];
    /* H4 = b2[3]<<4 | (b2[4] & 0x0F)
     * H5 = b2[5]<<4 | (b2[4] >> 4)   (both 12-bit signed) */
    cal.dig_H4 = (int16_t)(((int16_t)(int8_t)b2[3] << 4) | (b2[4] & 0x0F));
    cal.dig_H5 = (int16_t)(((int16_t)(int8_t)b2[5] << 4) | (b2[4] >> 4));
    cal.dig_H6 = (int8_t)b2[6];

    return 0;
}

static int probe_addr(uint8_t a)
{
    uint8_t id = 0;
    if (bb_i2c_write_then_read(a, BME_REG_ID, &id, 1) < 0) return 0;
    return (id == BME_CHIP_ID || id == 0x58) ? 1 : 0;
}

int bme280_init(void)
{
    s_present = 0;

    bb_i2c_init();

    /* Try common addresses */
    if (probe_addr(0x76)) s_addr = 0x76;
    else if (probe_addr(0x77)) s_addr = 0x77;
    else return -1;

    /* Soft reset */
    bb_i2c_write_reg8(s_addr, BME_REG_RESET, 0xB6);
    for (volatile int i = 0; i < 100000; i++) __asm__("nop");

    if (read_calibration() < 0) return -1;

    /* ctrl_hum must be written BEFORE ctrl_meas to take effect */
    if (bb_i2c_write_reg8(s_addr, BME_REG_CTRL_HUM,  BME_CTRL_HUM_X1)    < 0) return -1;
    if (bb_i2c_write_reg8(s_addr, BME_REG_CONFIG,    BME_CONFIG_DEFAULT) < 0) return -1;

    s_present = 1;
    return 0;
}

int bme280_present(void) { return s_present; }

/* Bosch reference: temperature compensation, returns °C × 100 (cdeg). */
static int32_t compensate_T_int100(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)cal.dig_T1 << 1))) * ((int32_t)cal.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)cal.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)cal.dig_T1))) >> 12) *
            ((int32_t)cal.dig_T3)) >> 14;
    cal.t_fine = var1 + var2;
    T = (cal.t_fine * 5 + 128) >> 8;   /* °C × 100 */
    return T;
}

/* Bosch reference humidity compensation, output in Q22.10 (%RH × 1024).
 * We rescale to %RH × 100 (centi-percent). */
static uint32_t compensate_H_q22_10(int32_t adc_H)
{
    int32_t v_x1_u32r;
    v_x1_u32r = (cal.t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)cal.dig_H4) << 20) -
                    (((int32_t)cal.dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15) *
                 (((((((v_x1_u32r * ((int32_t)cal.dig_H6)) >> 10) *
                      (((v_x1_u32r * ((int32_t)cal.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
                    ((int32_t)2097152)) * ((int32_t)cal.dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                               ((int32_t)cal.dig_H1)) >> 4));
    if (v_x1_u32r < 0)         v_x1_u32r = 0;
    if (v_x1_u32r > 419430400) v_x1_u32r = 419430400;
    return (uint32_t)(v_x1_u32r >> 12);   /* %RH × 1024 */
}

int bme280_sample(void)
{
    if (!s_present) return -1;

    /* Trigger one forced-mode conversion */
    if (bb_i2c_write_reg8(s_addr, BME_REG_CTRL_MEAS, BME_CTRL_MEAS_FORCE) < 0)
        return -1;

    /* Poll status: bit3 = measuring. ~10 ms typical for T x1 + H x1. */
    uint8_t st = 0;
    for (int i = 0; i < 50; i++) {
        for (volatile int d = 0; d < 20000; d++) __asm__("nop");
        if (bb_i2c_write_then_read(s_addr, BME_REG_STATUS, &st, 1) < 0) return -1;
        if ((st & 0x08) == 0) break;
    }

    uint8_t raw[8];
    if (bb_i2c_write_then_read(s_addr, BME_REG_PRESS_MSB, raw, 8) < 0)
        return -1;

    /* Press: raw[0..2] (ignored). Temp: raw[3..5] 20-bit. Hum: raw[6..7] 16-bit. */
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);
    int32_t adc_H = ((int32_t)raw[6] << 8)  |  (int32_t)raw[7];

    int32_t  T100 = compensate_T_int100(adc_T);
    uint32_t Hq   = compensate_H_q22_10(adc_H);

    /* Hq is %RH × 1024 → centi-percent = Hq * 100 / 1024.
     * Use rounding division. */
    uint32_t H100 = (Hq * 100u + 512u) >> 10;
    if (H100 > 10000) H100 = 10000;

    s_temp_cdeg = (int16_t)T100;
    s_hum_cpct  = (uint16_t)H100;
    return 0;
}

int16_t  bme280_get_temp_cdeg(void) { return s_temp_cdeg; }
uint16_t bme280_get_hum_cpct(void)  { return s_hum_cpct;  }
