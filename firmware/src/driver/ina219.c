/*
 * ina219.c — INA219 bus/shunt voltage reading + charger status GPIOs
 *
 * INA219 on I2C2 at address 0x40 (A0=A1=GND), PB13=SCL, PB14=SDA.
 * Shunt voltage register (0x01): signed 16-bit, LSB = 10µV.
 * Bus voltage register (0x02): bits [15:3] = voltage, LSB = 4 mV.
 *
 * Charge status (TP4056-style):
 *   PA10 (CHRG):  low = charging
 *   PA8  (STDBY): low = charge complete
 *   Both high = no power / no battery
 *   Both low  = fault
 */

#include "ina219.h"
#include "i2c.h"
#include "stm32u0.h"
#include "hw_pins.h"

#define INA219_ADDR         0x40

/* INA219 registers */
#define INA219_REG_CONFIG   0x00
#define INA219_REG_SHUNT_V  0x01
#define INA219_REG_BUS_V    0x02

/* Default config: 32V range, 320mV shunt, 12-bit, continuous */
#define INA219_CONFIG_DEFAULT  0x399F

void ina219_init(void)
{
    /* Enable GPIOA clock for charge status pins + SENSE_LDO_EN (PA15) */
    RCC_IOPENR |= (1 << 0);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* PA15 = SENSE_LDO_EN (TPS7A02 enable). Drive HIGH to power VCC_SENSE.
     * Without this, R38/R39 (4.7k pull-ups to VCC_SENSE) act as pull-DOWNs
     * against R40/R41 (4.7k pull-ups to VCC) and the I2C bus sits at ~VCC/2.
     * Must be set BEFORE i2c2_init(). */
    GPIO_MODER(GPIOA_BASE) &= ~(3u << (15 * 2));
    GPIO_MODER(GPIOA_BASE) |=  (1u << (15 * 2));   /* output */
    GPIO_OTYPER(GPIOA_BASE) &= ~(1u << 15);        /* push-pull */
    GPIO_BSRR(GPIOA_BASE)    =  (1u << 15);        /* set HIGH */
    /* LDO needs ~1ms soft-start + caps need to charge */
    for (volatile int i = 0; i < 200000; i++) __asm__("nop");

    /* Configure PA10 (CHRG) as input with pull-up */
    GPIO_MODER(GPIOA_BASE) &= ~(3 << (BAT_CHRG_PIN * 2));   /* input */
    uint32_t p = GPIO_PUPDR(GPIOA_BASE);
    p &= ~(3 << (BAT_CHRG_PIN * 2));
    p |=  (1 << (BAT_CHRG_PIN * 2));   /* pull-up */
    GPIO_PUPDR(GPIOA_BASE) = p;

    /* Configure PA8 (STDBY) as input with pull-up */
    GPIO_MODER(GPIOA_BASE) &= ~(3 << (BAT_STDBY_PIN * 2));  /* input */
    p = GPIO_PUPDR(GPIOA_BASE);
    p &= ~(3 << (BAT_STDBY_PIN * 2));
    p |=  (1 << (BAT_STDBY_PIN * 2));  /* pull-up */
    GPIO_PUPDR(GPIOA_BASE) = p;

    /* Init I2C2 (INA219 is on I2C2: PB13/PB14) */
    i2c2_init();

    /* Soft-reset INA219 (bit 15 of CONFIG). Self-clears when complete. */
    i2c2_write_reg(INA219_ADDR, INA219_REG_CONFIG, 0x8000);
    for (volatile int i = 0; i < 100000; i++) __asm__("nop");

    /* Write default configuration to INA219 */
    i2c2_write_reg(INA219_ADDR, INA219_REG_CONFIG, INA219_CONFIG_DEFAULT);

    /* Allow first conversion to complete (~1 ms at 12-bit, single sample) */
    for (volatile int i = 0; i < 100000; i++) __asm__("nop");
}

int16_t ina219_read_shunt_mv(void)
{
    uint16_t raw = 0;
    if (i2c2_read_reg(INA219_ADDR, INA219_REG_SHUNT_V, &raw) != 0)
        return 0;

    /* raw is signed 16-bit, LSB = 10µV.
     * shunt_mv = raw * 10 / 1000 = raw / 100 */
    int16_t shunt_mv = (int16_t)(((int32_t)(int16_t)raw * 10) / 1000);
    return shunt_mv;
}

int32_t ina219_read_shunt_uv(void)
{
    uint16_t raw = 0;
    if (i2c2_read_reg(INA219_ADDR, INA219_REG_SHUNT_V, &raw) != 0)
        return 0;
    /* signed 16-bit, LSB = 10 µV */
    return (int32_t)(int16_t)raw * 10;
}

uint16_t ina219_read_bus_mv(void)
{
    uint16_t raw = 0;
    if (i2c2_read_reg(INA219_ADDR, INA219_REG_BUS_V, &raw) != 0)
        return 0;

    /* Bits [15:3] contain the voltage, LSB = 4 mV.
     * Shift right by 3, multiply by 4. */
    uint16_t mv = (raw >> 3) * 4;
    return mv;
}

ChargeStatus charge_get_status(void)
{
    uint32_t idr = GPIO_IDR(GPIOA_BASE);
    uint8_t chrg  = (idr >> BAT_CHRG_PIN) & 1;   /* 0 = charging */
    uint8_t stdby = (idr >> BAT_STDBY_PIN) & 1;   /* 0 = done */

    if (!chrg && stdby)
        return CHARGE_CHARGING;
    if (chrg && !stdby)
        return CHARGE_DONE;
    if (!chrg && !stdby)
        return CHARGE_FAULT;
    return CHARGE_OFF;  /* both high */
}

const char *charge_status_str(ChargeStatus s)
{
    switch (s) {
    case CHARGE_CHARGING: return "Charging";
    case CHARGE_DONE:     return "Done";
    case CHARGE_FAULT:    return "Fault";
    default:              return "Off";
    }
}
