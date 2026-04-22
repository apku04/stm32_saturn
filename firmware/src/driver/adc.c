/*
 * adc.c — ADC1 driver for battery voltage measurement
 *
 * Hardware: voltage divider from VBAT through sense LDO (PA15 enable)
 *           to PB4 (ADC1_IN22).  Divider ratio assumed 1:1 (1M/1M).
 *           BSENSE = VBAT / 2
 *
 * ADC: 12-bit, VDDA (3.3V) reference
 * Formula: battery_mV = raw * 3300 / 4096 * 2  =  raw * 6600 / 4096
 */

#include "adc.h"
#include "stm32u0.h"
#include "hw_pins.h"

#define ADC_CHANNEL_BAT  22   /* PB4 = ADC1_IN22 on STM32U073 */

void adc_init(void)
{
    /* Enable ADC clock (bit 20 of RCC_APBENR2) */
    RCC_APBENR2 |= (1 << 20);

    /* Enable GPIOB clock (already done in main, but be safe) */
    RCC_IOPENR |= (1 << 1);

    /* Enable GPIOA clock for LDO enable pin */
    RCC_IOPENR |= (1 << 0);

    /* Configure PB4 as analog (MODER = 11) */
    GPIO_MODER(GPIOB_BASE) |= (3 << (BAT_SENSE_PIN * 2));

    /* Configure PA15 as output for sense LDO enable */
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(3 << (SENSE_LDO_EN_PIN * 2)))
                            | (1 << (SENSE_LDO_EN_PIN * 2));

    /* Enable sense LDO permanently (low power, avoids settle time issues) */
    GPIO_BSRR(GPIOA_BASE) = (1 << SENSE_LDO_EN_PIN);

    /* Make sure ADC is disabled before configuration */
    if (ADC1_CR & ADC_CR_ADEN) {
        ADC1_CR |= ADC_CR_ADDIS;
        while (ADC1_CR & ADC_CR_ADDIS);
    }

    /* Enable ADC voltage regulator */
    ADC1_CR |= ADC_CR_ADVREGEN;
    /* Wait for regulator startup (~20us) */
    for (volatile int i = 0; i < 500; i++);

    /* Calibrate */
    ADC1_CR |= ADC_CR_ADCAL;
    while (ADC1_CR & ADC_CR_ADCAL);

    /* CFGR1: 12-bit resolution (default), single conversion, software trigger */
    ADC1_CFGR1 = 0;

    /* SMPR: slowest sampling time for stable battery reading (160.5 ADC clocks) */
    ADC1_SMPR = 7;  /* SMP = 111 */

    /* Select channel 22 (PB4) */
    ADC1_CHSELR = (1 << ADC_CHANNEL_BAT);

    /* Enable ADC */
    ADC1_ISR = ADC_ISR_ADRDY;  /* clear ready flag */
    ADC1_CR |= ADC_CR_ADEN;
    while (!(ADC1_ISR & ADC_ISR_ADRDY));
}

uint16_t adc_read_battery_raw(void)
{
    /* Average 8 samples for stability (high-impedance divider) */
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        ADC1_ISR = ADC_ISR_EOC;
        ADC1_CR |= ADC_CR_ADSTART;
        while (!(ADC1_ISR & ADC_ISR_EOC));
        sum += (ADC1_DR & 0xFFF);
    }
    return (uint16_t)(sum / 8);
}

uint16_t adc_read_battery_mv(void)
{
    uint16_t raw = adc_read_battery_raw();
    /* ADC pin voltage = raw * 3300 / 4096 mV
     * Battery voltage = pin * 2 (1:1 divider)
     * = raw * 6600 / 4096 */
    uint32_t mv = ((uint32_t)raw * 6600UL) / 4096UL;
    return (uint16_t)mv;
}
