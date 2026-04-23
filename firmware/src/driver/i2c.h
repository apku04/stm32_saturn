/*
 * i2c.h — I2C1 & I2C2 master driver for STM32U073
 *
 * I2C1: PB8/PB9 (AF4)
 * I2C2: PB13/PB14 (AF6) — INA219 current sensor
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>

/* I2C1 (PB8=SCL, PB9=SDA, AF4) */
void    i2c_init(void);
int     i2c_write(uint8_t addr, const uint8_t *data, uint8_t len);
int     i2c_read(uint8_t addr, uint8_t *data, uint8_t len);
int     i2c_write_reg(uint8_t addr, uint8_t reg, uint16_t val);
int     i2c_read_reg(uint8_t addr, uint8_t reg, uint16_t *val);

/* I2C2 (PB13=SCL, PB14=SDA, AF6) */
void    i2c2_init(void);
int     i2c2_write(uint8_t addr, const uint8_t *data, uint8_t len);
int     i2c2_read(uint8_t addr, uint8_t *data, uint8_t len);
int     i2c2_write_reg(uint8_t addr, uint8_t reg, uint16_t val);
int     i2c2_read_reg(uint8_t addr, uint8_t reg, uint16_t *val);

#endif /* I2C_H */
