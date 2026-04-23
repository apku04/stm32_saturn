/*
 * ina219.c — INA219 bus voltage reading + charger status GPIOs
 *
 * INA219 on I2C2 at address 0x40 (A0=A1=GND), PB8=SCL, PB9=SDA.
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
#define INA219_REG_BUS_V    0x02

/* Default config: 32V range, 320mV shunt, 12-bit, continuous */
#define INA219_CONFIG_DEFAULT  0x399F

void ina219_init(void)
{
    /* Enable GPIOA clock for charge status pins */
    RCC_IOPENR |= (1 << 0);
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

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

    /* Init I2C */
    i2c_init();

    /* Write default configuration to INA219 */
    i2c_write_reg(INA219_ADDR, INA219_REG_CONFIG, INA219_CONFIG_DEFAULT);
}

uint16_t ina219_read_bus_mv(void)
{
    uint16_t raw = 0;
    if (i2c_read_reg(INA219_ADDR, INA219_REG_BUS_V, &raw) != 0)
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
