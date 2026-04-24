/*
 * adc.c — ADC1 driver for STM32U073
 *
 * Provides a real ADC1 init + single-channel read.
 *
 * Battery sense (PB4) has no ADC channel on this MCU — the existing
 * HW divider lands on a non-ADC pin.  Battery reads still return 0
 * until a hardware bodge moves BAT_SENSE to an ADC-capable pin.
 *
 * However ADC + math is exercised here via the internal channels:
 *   - VREFINT  (ch 13): factory-trimmed bandgap (~1.212 V) — used to
 *                       compute actual VDDA.
 *   - TEMP     (ch 12): on-chip temperature sensor.
 *
 * Use `get vrefint` / `get vdda` from the terminal to verify the
 * peripheral is alive end-to-end.
 */

#include "adc.h"
#include "stm32u0.h"
#include "hw_pins.h"

/* RCC_APBENR2: ADC1EN = bit 20 */
#define RCC_APBENR2_ADC1EN  (1u << 20)

/* ADC_CCR bits */
#define ADC_CCR_VREFEN      (1u << 22)
#define ADC_CCR_TSEN        (1u << 23)

/* ADC_CFGR2 CKMODE bits [31:30]:
 *   00 = async (needs CCIPR clock source)
 *   01 = HCLK/2 sync
 *   10 = HCLK/4 sync
 *   11 = HCLK/1 sync                                            */
#define ADC_CFGR2_CKMODE_HCLK_DIV4  (2u << 30)

/* Internal channel numbers on STM32U0 */
#define ADC_CH_TEMP         12
#define ADC_CH_VREFINT      13

/* Factory VREFINT calibration — 16-bit raw at VDDA=3.0V, T≈30°C.
 * Address verified empirically on STM32U073CBT6 (RM0503). */
#define VREFINT_CAL_ADDR    0x1FFF6E68u
#define VREFINT_CAL         (*(volatile uint16_t *)VREFINT_CAL_ADDR)
#define VREFINT_CAL_VDDA_MV 3000u

static uint8_t s_adc_ready = 0;

static void adc_calibrate(void)
{
    /* Calibration must run with ADEN=0 */
    if (ADC1_CR & ADC_CR_ADEN) {
        ADC1_CR |= ADC_CR_ADDIS;
        while (ADC1_CR & ADC_CR_ADEN) { }
    }
    ADC1_CR |= ADC_CR_ADCAL;
    while (ADC1_CR & ADC_CR_ADCAL) { }
}

void adc_init(void)
{
    /* Enable GPIOA clock for LDO enable pin */
    RCC_IOPENR |= (1 << 0);

    /* Configure PA15 as output for sense LDO enable */
    GPIO_MODER(GPIOA_BASE) = (GPIO_MODER(GPIOA_BASE) & ~(3 << (SENSE_LDO_EN_PIN * 2)))
                            | (1 << (SENSE_LDO_EN_PIN * 2));
    GPIO_BSRR(GPIOA_BASE) = (1 << SENSE_LDO_EN_PIN);

    /* Enable ADC1 peripheral clock */
    RCC_APBENR2 |= RCC_APBENR2_ADC1EN;
    for (volatile int i = 0; i < 10; i++) __asm__("nop");

    /* Enable internal channels (VREFINT, TEMP). PRESC unused in sync mode. */
    ADC1_CCR = ADC_CCR_VREFEN | ADC_CCR_TSEN;

    /* Enable internal voltage regulator and wait t_ADCVREG_SETUP (~20 µs) */
    ADC1_CR = ADC_CR_ADVREGEN;
    for (volatile int i = 0; i < 2000; i++) __asm__("nop");

    /* Calibrate */
    adc_calibrate();

    /* CFGR1 default: 12-bit, single conversion, software trigger.
     * CFGR2: synchronous clock from HCLK/4 (works without RCC_CCIPR setup). */
    ADC1_CFGR1 = 0;
    ADC1_CFGR2 = ADC_CFGR2_CKMODE_HCLK_DIV4;

    /* Sampling time — longest available (SMP1 = 0b111 ≈ 160.5 cycles)
       Internal channels (VREFINT, TEMP) need long sampling. */
    ADC1_SMPR  = 0x7;

    /* Enable ADC */
    ADC1_ISR   = ADC_ISR_ADRDY;          /* clear ADRDY */
    ADC1_CR   |= ADC_CR_ADEN;
    while (!(ADC1_ISR & ADC_ISR_ADRDY)) { }

    s_adc_ready = 1;
}

uint16_t adc_read_channel_raw(uint8_t ch)
{
    if (!s_adc_ready || ch > 18) return 0;

    /* Select single channel */
    ADC1_CHSELR = (1u << ch);

    /* For internal channels, allow tiny stabilisation */
    if (ch == ADC_CH_VREFINT || ch == ADC_CH_TEMP) {
        for (volatile int i = 0; i < 200; i++) __asm__("nop");
    }

    /* Clear EOC and start */
    ADC1_ISR = ADC_ISR_EOC;
    ADC1_CR |= ADC_CR_ADSTART;

    /* Wait for end of conversion (timeout ~ a few ms) */
    uint32_t timeout = 200000;
    while (!(ADC1_ISR & ADC_ISR_EOC)) {
        if (--timeout == 0) return 0;
    }

    return (uint16_t)(ADC1_DR & 0xFFFF);
}

/* Read VREFINT raw — useful to prove ADC works without external pins. */
uint16_t adc_read_vrefint_raw(void)
{
    return adc_read_channel_raw(ADC_CH_VREFINT);
}

/* Compute actual VDDA in mV using factory VREFINT calibration.
 *   VDDA = 3000 mV * VREFINT_CAL / VREFINT_DATA          */
uint16_t adc_vdda_from_raw(uint16_t raw)
{
    if (raw == 0) return 0;
    uint32_t vdda = (uint32_t)VREFINT_CAL_VDDA_MV * (uint32_t)VREFINT_CAL / (uint32_t)raw;
    if (vdda > 0xFFFF) vdda = 0xFFFF;
    return (uint16_t)vdda;
}

uint16_t adc_read_vdda_mv(void)
{
    return adc_vdda_from_raw(adc_read_vrefint_raw());
}

uint16_t adc_read_battery_raw(void)
{
    /* PB4 has no ADC channel on STM32U073.
       Returns 0 until BAT_SENSE is rerouted to an ADC pin (bodge). */
    return 0;
}

uint16_t adc_read_battery_mv(void)
{
    return 0;
}
