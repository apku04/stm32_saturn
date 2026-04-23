/*
 * i2c.h — I2C1 master driver for STM32U073
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>

void    i2c_init(void);
int     i2c_write(uint8_t addr, const uint8_t *data, uint8_t len);
int     i2c_read(uint8_t addr, uint8_t *data, uint8_t len);
int     i2c_write_reg(uint8_t addr, uint8_t reg, uint16_t val);
int     i2c_read_reg(uint8_t addr, uint8_t reg, uint16_t *val);

#endif /* I2C_H */
