/*
 * adc.c — ADC stub (battery sense not usable on this PCB revision)
 *
 * HARDWARE LIMITATION: BAT_SENSE is routed to PB4, which has NO ADC
 * input on STM32U073CBT6.  The MCU VBAT pin is tied to the 3.3V rail
 * (not battery).  All ADC-capable pins (PA0–PA7, PB0, PB1) are
 * consumed by LoRa SPI and GPS UART.
 *
 * Battery voltage is read via INA219 bus voltage register instead.
 * See beaconHandler() in main.c.
 *
 * This file is kept as a stub so the build links and future PCB
 * revisions can add real ADC channels.
 */

#include "adc.h"
#include "stm32u0.h"
#include "hw_pins.h"

void adc_init(void)
{
    /* Enable GPIOA clock for LDO enable pin */
    RCC_IOPENR |= (1 << 0);

    /* Configure PA15 as output for sense LDO enable (powers sensor divider) */
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(3 << (SENSE_LDO_EN_PIN * 2)))
                            | (1 << (SENSE_LDO_EN_PIN * 2));

    /* Enable sense LDO — other sensors on this rail may need it */
    GPIO_BSRR(GPIOA_BASE) = (1 << SENSE_LDO_EN_PIN);

    /* ADC peripheral left uninitialised — no usable channel on this PCB */
}

uint16_t adc_read_battery_raw(void)
{
    /* PB4 has no ADC on STM32U073 — cannot read battery sense */
    return 0;
}

uint16_t adc_read_battery_mv(void)
{
    /* PB4 has no ADC on STM32U073 — cannot read battery sense.
       Use ina219_read_bus_mv() for battery voltage instead. */
    return 0;
}

uint16_t adc_read_channel_raw(uint8_t ch)
{
    (void)ch;
    /* ADC not initialised — stub for future PCB revisions */
    return 0;
}
