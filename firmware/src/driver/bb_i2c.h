/*
 * bb_i2c.h — Bit-bang I2C master on PB6 (SCL) / PB7 (SDA)
 *
 * The hardware I2C1 peripheral is already used on PB8/PB9 for the on-board
 * INA219, so the external I2C header (PB6/PB7) is driven in software so a
 * second device (BME280) can sit on its own bus without peripheral
 * conflicts.  ~50 kHz, polled, no clock stretching support beyond a short
 * timeout.
 */

#ifndef BB_I2C_H
#define BB_I2C_H

#include <stdint.h>

void bb_i2c_init(void);

/* Returns 0 on ACK for every byte, -1 on NACK / bus error. */
int  bb_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len);
int  bb_i2c_read (uint8_t addr7,       uint8_t *data, uint16_t len);

/* Convenience: write one register address then read N bytes (repeated start). */
int  bb_i2c_write_then_read(uint8_t addr7, uint8_t reg,
                            uint8_t *data, uint16_t len);

/* Convenience: write one register followed by one data byte. */
int  bb_i2c_write_reg8(uint8_t addr7, uint8_t reg, uint8_t val);

/* Diagnostic: release both lines and report their idle levels.
 * Both should read 1 on a healthy bus (pull-ups + no slave holding low).
 * scl=0 means SCL is shorted/held low (unrecoverable in software).
 * sda=0 means a slave is mid-byte; bb_i2c_init's 9-clock recovery should
 * have unstuck it — if it persists, slave is dead or pull-up is missing. */
void bb_i2c_probe_idle(uint8_t *scl, uint8_t *sda);

#endif /* BB_I2C_H */
