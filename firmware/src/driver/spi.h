/*
 * spi.h — SPI1 driver for STM32U073 (bare-metal)
 */

#ifndef SPI_H
#define SPI_H

#include <stdint.h>

void     spi_init(void);
uint8_t  spi_transfer(uint8_t tx);
void     spi_exchange_buffer(uint8_t *tx, uint16_t len, uint8_t *rx);

#endif /* SPI_H */
