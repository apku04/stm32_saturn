/*
 * sht3x.h — Sensirion SHT3x (SHT30/31/35) driver
 *
 * I2C address 0x44 (ADDR pin low, default) or 0x45 (high).
 * Sits on the external I2C header (PB6/PB7) via the bit-bang driver.
 *
 * Output units (match what the LoRa beacon already ships):
 *   temperature: int16_t centi-°C   (e.g. 2345 = 23.45 °C)
 *   humidity:    uint16_t centi-%RH (e.g. 4567 = 45.67 %)
 */

#ifndef SHT3X_H
#define SHT3X_H

#include <stdint.h>

int  sht3x_init(void);                  /* returns 0 if a sensor is found */
int  sht3x_present(void);               /* 1 if init succeeded            */
int  sht3x_sample(void);                /* triggers single-shot conv.     */

int16_t  sht3x_get_temp_cdeg(void);
uint16_t sht3x_get_hum_cpct(void);

#endif /* SHT3X_H */
