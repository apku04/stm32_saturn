/*
 * bme280.h — Bosch BME280 temperature / humidity / pressure sensor driver
 *
 * Sits on the external I2C header (PB6/PB7) via the bit-bang driver.
 * Default 7-bit address 0x76 (SDO tied to GND); the driver also auto-tries
 * 0x77 if the first probe fails.
 *
 * Returned units (matches what the LoRa beacon ships):
 *   temperature: int16_t centi-°C   (e.g. 2345 = 23.45 °C)
 *   humidity:    uint16_t centi-%RH (e.g. 4567 = 45.67 %)
 */

#ifndef BME280_H
#define BME280_H

#include <stdint.h>

int  bme280_init(void);                 /* returns 0 if a sensor is found */
int  bme280_present(void);              /* 1 if init succeeded, else 0    */

/* Trigger a forced-mode measurement and update the cached values.
 * Blocks until the conversion completes (~10 ms). Returns 0 on success. */
int  bme280_sample(void);

int16_t  bme280_get_temp_cdeg(void);    /* last sampled temperature       */
uint16_t bme280_get_hum_cpct(void);     /* last sampled humidity          */

#endif /* BME280_H */
