/*
 * bb_i2c.c — Software I2C master on PB6 (SCL) / PB7 (SDA)
 *
 * Pins are driven open-drain by toggling MODER between input (released,
 * pulled high by internal/external pull-ups) and output-low (ODR=0).
 * Internal pull-ups are enabled as a fallback in case the BME280 break-out
 * board does not include them.
 */

#include "bb_i2c.h"
#include "stm32u0.h"
#include "hw_pins.h"

#define SCL_PIN  I2C_SCL_PIN   /* PB6 */
#define SDA_PIN  I2C_SDA_PIN   /* PB7 */

/* ~10 µs per half-bit at 16 MHz HSI → ~50 kHz SCL. Tuned by NOP count. */
#define BB_DELAY()  do { for (volatile int _i = 0; _i < 30; _i++) __asm__("nop"); } while (0)

static inline void scl_low(void)  { GPIO_MODER(GPIOB_BASE) |=  (1u << (SCL_PIN * 2)); }   /* output */
static inline void scl_high(void) { GPIO_MODER(GPIOB_BASE) &= ~(3u << (SCL_PIN * 2)); }   /* input → pull-up */
static inline void sda_low(void)  { GPIO_MODER(GPIOB_BASE) |=  (1u << (SDA_PIN * 2)); }
static inline void sda_high(void) { GPIO_MODER(GPIOB_BASE) &= ~(3u << (SDA_PIN * 2)); }
static inline uint8_t sda_read(void) {
    return (uint8_t)((GPIO_IDR(GPIOB_BASE) >> SDA_PIN) & 1u);
}

/* Wait for SCL to actually go high (basic clock-stretching tolerance). */
static int scl_wait_high(void)
{
    scl_high();
    for (int i = 0; i < 1000; i++) {
        if ((GPIO_IDR(GPIOB_BASE) >> SCL_PIN) & 1u)
            return 0;
        BB_DELAY();
    }
    return -1;
}

void bb_i2c_probe_idle(uint8_t *scl, uint8_t *sda)
{
    /* Release both lines (switch back to input-with-pull-up). */
    sda_high();
    scl_high();
    /* Settle: rise time on a 4.7k pull-up against ~10pF is ~50ns; give
     * plenty of margin for board capacitance + slow scope probes. */
    for (volatile int i = 0; i < 1000; i++) __asm__("nop");
    uint32_t idr = GPIO_IDR(GPIOB_BASE);
    if (scl) *scl = (uint8_t)((idr >> SCL_PIN) & 1u);
    if (sda) *sda = (uint8_t)((idr >> SDA_PIN) & 1u);
}

void bb_i2c_init(void)
{
    /* Enable GPIOB clock */
    RCC_IOPENR |= (1u << 1);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Pre-load ODR=0 so switching to output mode pulls the line low. */
    GPIO_BSRR(GPIOB_BASE) = (1u << (SCL_PIN + 16)) | (1u << (SDA_PIN + 16));

    /* Open-drain (matters when MODER=output) */
    GPIO_OTYPER(GPIOB_BASE) |= (1u << SCL_PIN) | (1u << SDA_PIN);

    /* Internal pull-ups as fallback (external 4.7 k preferred). */
    uint32_t p = GPIO_PUPDR(GPIOB_BASE);
    p &= ~((3u << (SCL_PIN * 2)) | (3u << (SDA_PIN * 2)));
    p |=  ((1u << (SCL_PIN * 2)) | (1u << (SDA_PIN * 2)));
    GPIO_PUPDR(GPIOB_BASE) = p;

    /* Start in released (input) state → lines float high via pull-ups. */
    sda_high();
    scl_high();
    BB_DELAY();

    /* Bus-recovery: clock 9 pulses with SDA released to free a stuck slave. */
    for (int i = 0; i < 9; i++) {
        scl_low();  BB_DELAY();
        scl_high(); BB_DELAY();
    }
    /* Generate a STOP to leave the bus idle. */
    sda_low();  BB_DELAY();
    scl_high(); BB_DELAY();
    sda_high(); BB_DELAY();
}

static void bb_start(void)
{
    sda_high(); scl_high(); BB_DELAY();
    sda_low();  BB_DELAY();
    scl_low();  BB_DELAY();
}

static void bb_repeated_start(void)
{
    sda_high(); BB_DELAY();
    scl_wait_high(); BB_DELAY();
    sda_low(); BB_DELAY();
    scl_low(); BB_DELAY();
}

static void bb_stop(void)
{
    sda_low();  BB_DELAY();
    scl_wait_high(); BB_DELAY();
    sda_high(); BB_DELAY();
}

/* Returns 0 on ACK, 1 on NACK, -1 on bus error. */
static int bb_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_high();
        else               sda_low();
        BB_DELAY();
        if (scl_wait_high() < 0) return -1;
        BB_DELAY();
        scl_low();
        BB_DELAY();
    }
    /* ACK clock */
    sda_high();          /* release SDA so slave can drive */
    BB_DELAY();
    if (scl_wait_high() < 0) return -1;
    BB_DELAY();
    int ack = sda_read() == 0 ? 0 : 1;
    scl_low();
    BB_DELAY();
    return ack;
}

static uint8_t bb_read_byte(int ack)
{
    uint8_t v = 0;
    sda_high();   /* release */
    for (int i = 7; i >= 0; i--) {
        BB_DELAY();
        if (scl_wait_high() < 0) return 0;
        BB_DELAY();
        if (sda_read()) v |= (1u << i);
        scl_low();
    }
    /* Send ACK / NACK */
    if (ack) sda_low(); else sda_high();
    BB_DELAY();
    scl_wait_high();
    BB_DELAY();
    scl_low();
    sda_high();
    BB_DELAY();
    return v;
}

int bb_i2c_write(uint8_t addr7, const uint8_t *data, uint16_t len)
{
    bb_start();
    if (bb_write_byte((uint8_t)(addr7 << 1)) != 0) { bb_stop(); return -1; }
    for (uint16_t i = 0; i < len; i++) {
        if (bb_write_byte(data[i]) != 0) { bb_stop(); return -1; }
    }
    bb_stop();
    return 0;
}

int bb_i2c_read(uint8_t addr7, uint8_t *data, uint16_t len)
{
    bb_start();
    if (bb_write_byte((uint8_t)((addr7 << 1) | 1)) != 0) { bb_stop(); return -1; }
    for (uint16_t i = 0; i < len; i++) {
        data[i] = bb_read_byte(i < (len - 1) ? 1 : 0);
    }
    bb_stop();
    return 0;
}

int bb_i2c_write_then_read(uint8_t addr7, uint8_t reg,
                           uint8_t *data, uint16_t len)
{
    bb_start();
    if (bb_write_byte((uint8_t)(addr7 << 1)) != 0) { bb_stop(); return -1; }
    if (bb_write_byte(reg) != 0)                   { bb_stop(); return -1; }
    bb_repeated_start();
    if (bb_write_byte((uint8_t)((addr7 << 1) | 1)) != 0) { bb_stop(); return -1; }
    for (uint16_t i = 0; i < len; i++) {
        data[i] = bb_read_byte(i < (len - 1) ? 1 : 0);
    }
    bb_stop();
    return 0;
}

int bb_i2c_write_reg8(uint8_t addr7, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return bb_i2c_write(addr7, buf, 2);
}
