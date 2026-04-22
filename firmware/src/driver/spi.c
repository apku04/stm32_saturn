/*
 * spi.c — SPI1 driver for STM32U073 (bare-metal)
 *
 * SPI1 pins (from schematic):
 *   PA5 = SCK   (AF5)
 *   PA6 = MISO  (AF5)
 *   PA7 = MOSI  (AF5)
 *   PA4 = NSS   (GPIO output, directly driven)
 *
 * Clock: APB2 = 16MHz, SPI prescaler /8 => 2MHz SPI clock
 */

#include "spi.h"
#include "stm32u0.h"
#include "hw_pins.h"

void spi_init(void)
{
    /* Enable GPIOA clock (bit 0) */
    RCC_IOPENR |= (1 << 0);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* PA5 (SCK), PA6 (MISO), PA7 (MOSI) → AF5 (SPI1) */
    /* MODER: 10 = alternate function */
    uint32_t moder = GPIO_MODER(GPIOA_BASE);
    moder &= ~((3 << (5*2)) | (3 << (6*2)) | (3 << (7*2)));
    moder |=  ((2 << (5*2)) | (2 << (6*2)) | (2 << (7*2)));
    GPIO_MODER(GPIOA_BASE) = moder;

    /* AFRL: PA5=AF5, PA6=AF5, PA7=AF5 (bits 20-31) */
    uint32_t afrl = GPIO_AFRL(GPIOA_BASE);
    afrl &= ~((0xF << 20) | (0xF << 24) | (0xF << 28));
    afrl |=  ((5 << 20) | (5 << 24) | (5 << 28));
    GPIO_AFRL(GPIOA_BASE) = afrl;

    /* High speed for SPI pins */
    uint32_t ospeedr = GPIO_OSPEEDR(GPIOA_BASE);
    ospeedr |= (3 << (5*2)) | (3 << (6*2)) | (3 << (7*2));
    GPIO_OSPEEDR(GPIOA_BASE) = ospeedr;

    /* PA4 (NSS) → GPIO output, push-pull, start high */
    moder = GPIO_MODER(GPIOA_BASE);
    moder &= ~(3 << (4*2));
    moder |=  (1 << (4*2));   /* 01 = output */
    GPIO_MODER(GPIOA_BASE) = moder;
    GPIO_BSRR(GPIOA_BASE) = (1 << 4);  /* NSS high (deselected) */

    /* Enable SPI1 clock (APB2, bit 12) */
    RCC_APBENR2 |= (1 << 12);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Configure SPI1:
       - Master mode
       - Software slave management (SSM=1, SSI=1)
       - Baud rate = /8 (16MHz/8 = 2MHz)
       - CPOL=0, CPHA=0 (SX1262 default)
       - MSB first (default) */
    SPI1_CR1 = SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_MSTR | SPI_CR1_BR_DIV8;

    /* CR2: 8-bit data, FIFO threshold = 1 byte */
    SPI1_CR2 = SPI_CR2_FRXTH | SPI_CR2_DS_8BIT;

    /* Enable SPI */
    SPI1_CR1 |= SPI_CR1_SPE;
}

uint8_t spi_transfer(uint8_t tx)
{
    /* Wait for TXE */
    while (!(SPI1_SR & SPI_SR_TXE));
    *(volatile uint8_t *)&SPI1_DR = tx;
    /* Wait for RXNE */
    while (!(SPI1_SR & SPI_SR_RXNE));
    return *(volatile uint8_t *)&SPI1_DR;
}

void spi_exchange_buffer(uint8_t *tx, uint16_t len, uint8_t *rx)
{
    for (uint16_t i = 0; i < len; i++) {
        uint8_t r = spi_transfer(tx[i]);
        if (rx) rx[i] = r;
    }
}
