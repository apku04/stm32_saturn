/*
 * i2c.c — I2C1 master driver for STM32U073
 *
 * PB6 = I2C1_SCL (AF4), PB7 = I2C1_SDA (AF4)
 * Clock: HSI16 (16 MHz), 100 kHz standard mode
 */

#include "i2c.h"
#include "stm32u0.h"
#include "hw_pins.h"

/* Timing for 100 kHz @ 16 MHz HSI:
 * PRESC=3 (4 MHz prescaled), SCLDEL=4, SDADEL=2, SCLH=0x0F, SCLL=0x13 */
#define I2C_TIMING  0x30420F13U

#define I2C_TIMEOUT 100000

void i2c_init(void)
{
    /* Enable GPIOB clock */
    RCC_IOPENR |= (1 << 1);

    /* Enable I2C1 clock (bit 21 of APBENR1) */
    RCC_APBENR1 |= (1 << 21);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Configure PB6, PB7 as AF4 open-drain */
    uint32_t m = GPIO_MODER(GPIOB_BASE);
    m &= ~((3 << (I2C_SCL_PIN * 2)) | (3 << (I2C_SDA_PIN * 2)));
    m |=  ((2 << (I2C_SCL_PIN * 2)) | (2 << (I2C_SDA_PIN * 2)));  /* AF mode */
    GPIO_MODER(GPIOB_BASE) = m;

    /* Open-drain */
    GPIO_OTYPER(GPIOB_BASE) |= (1 << I2C_SCL_PIN) | (1 << I2C_SDA_PIN);

    /* Pull-ups (external pullups recommended, but enable internal as fallback) */
    uint32_t p = GPIO_PUPDR(GPIOB_BASE);
    p &= ~((3 << (I2C_SCL_PIN * 2)) | (3 << (I2C_SDA_PIN * 2)));
    p |=  ((1 << (I2C_SCL_PIN * 2)) | (1 << (I2C_SDA_PIN * 2)));  /* pull-up */
    GPIO_PUPDR(GPIOB_BASE) = p;

    /* AF4 for PB6, PB7 (AFRL register, pins 0-7) */
    uint32_t afrl = GPIO_AFRL(GPIOB_BASE);
    afrl &= ~((0xF << (I2C_SCL_PIN * 4)) | (0xF << (I2C_SDA_PIN * 4)));
    afrl |=  ((4 << (I2C_SCL_PIN * 4)) | (4 << (I2C_SDA_PIN * 4)));
    GPIO_AFRL(GPIOB_BASE) = afrl;

    /* Disable I2C before configuration */
    I2C1_CR1 = 0;

    /* Set timing */
    I2C1_TIMINGR = I2C_TIMING;

    /* Enable I2C */
    I2C1_CR1 = 1;  /* PE = 1 */
}

int i2c_write(uint8_t addr, const uint8_t *data, uint8_t len)
{
    /* Wait until bus is free */
    uint32_t timeout = I2C_TIMEOUT;
    while ((I2C1_ISR & I2C_ISR_BUSY) && --timeout);
    if (!timeout) return -1;

    /* Configure: 7-bit addr, write, NBYTES, AUTOEND */
    I2C1_CR2 = ((uint32_t)addr << 1)
             | ((uint32_t)len << 16)
             | I2C_CR2_AUTOEND
             | I2C_CR2_START;

    for (uint8_t i = 0; i < len; i++) {
        timeout = I2C_TIMEOUT;
        while (!(I2C1_ISR & I2C_ISR_TXIS) && --timeout) {
            if (I2C1_ISR & I2C_ISR_NACKF) {
                I2C1_ICR = I2C_ICR_NACKCF;
                return -2;
            }
        }
        if (!timeout) return -1;
        I2C1_TXDR = data[i];
    }

    /* Wait for STOP */
    timeout = I2C_TIMEOUT;
    while (!(I2C1_ISR & I2C_ISR_STOPF) && --timeout);
    I2C1_ICR = I2C_ICR_STOPCF;

    return 0;
}

int i2c_read(uint8_t addr, uint8_t *data, uint8_t len)
{
    uint32_t timeout = I2C_TIMEOUT;
    while ((I2C1_ISR & I2C_ISR_BUSY) && --timeout);
    if (!timeout) return -1;

    /* Configure: 7-bit addr, read, NBYTES, AUTOEND */
    I2C1_CR2 = ((uint32_t)addr << 1)
             | ((uint32_t)len << 16)
             | I2C_CR2_RD_WRN
             | I2C_CR2_AUTOEND
             | I2C_CR2_START;

    for (uint8_t i = 0; i < len; i++) {
        timeout = I2C_TIMEOUT;
        while (!(I2C1_ISR & I2C_ISR_RXNE) && --timeout) {
            if (I2C1_ISR & I2C_ISR_NACKF) {
                I2C1_ICR = I2C_ICR_NACKCF;
                return -2;
            }
        }
        if (!timeout) return -1;
        data[i] = (uint8_t)I2C1_RXDR;
    }

    timeout = I2C_TIMEOUT;
    while (!(I2C1_ISR & I2C_ISR_STOPF) && --timeout);
    I2C1_ICR = I2C_ICR_STOPCF;

    return 0;
}

int i2c_write_reg(uint8_t addr, uint8_t reg, uint16_t val)
{
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (uint8_t)(val >> 8);    /* MSB first (INA219 is big-endian) */
    buf[2] = (uint8_t)(val & 0xFF);
    return i2c_write(addr, buf, 3);
}

int i2c_read_reg(uint8_t addr, uint8_t reg, uint16_t *val)
{
    /* Write register pointer (no AUTOEND — use restart) */
    uint32_t timeout = I2C_TIMEOUT;
    while ((I2C1_ISR & I2C_ISR_BUSY) && --timeout);
    if (!timeout) return -1;

    /* Write phase: 1 byte (register address), no AUTOEND */
    I2C1_CR2 = ((uint32_t)addr << 1)
             | (1U << 16)      /* NBYTES = 1 */
             | I2C_CR2_START;

    timeout = I2C_TIMEOUT;
    while (!(I2C1_ISR & I2C_ISR_TXIS) && --timeout) {
        if (I2C1_ISR & I2C_ISR_NACKF) {
            I2C1_ICR = I2C_ICR_NACKCF;
            return -2;
        }
    }
    if (!timeout) return -1;
    I2C1_TXDR = reg;

    /* Wait for transfer complete (TC, not STOPF since no AUTOEND) */
    timeout = I2C_TIMEOUT;
    while (!(I2C1_ISR & I2C_ISR_TC) && --timeout);
    if (!timeout) return -1;

    /* Read phase: 2 bytes with AUTOEND */
    I2C1_CR2 = ((uint32_t)addr << 1)
             | (2U << 16)
             | I2C_CR2_RD_WRN
             | I2C_CR2_AUTOEND
             | I2C_CR2_START;

    uint8_t buf[2];
    for (int i = 0; i < 2; i++) {
        timeout = I2C_TIMEOUT;
        while (!(I2C1_ISR & I2C_ISR_RXNE) && --timeout);
        if (!timeout) return -1;
        buf[i] = (uint8_t)I2C1_RXDR;
    }

    timeout = I2C_TIMEOUT;
    while (!(I2C1_ISR & I2C_ISR_STOPF) && --timeout);
    I2C1_ICR = I2C_ICR_STOPCF;

    *val = ((uint16_t)buf[0] << 8) | buf[1];  /* big-endian */
    return 0;
}
