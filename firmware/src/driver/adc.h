#ifndef ADC_H
#define ADC_H

#include <stdint.h>

void     adc_init(void);
uint16_t adc_read_battery_raw(void);
uint16_t adc_read_battery_mv(void);
uint16_t adc_read_channel_raw(uint8_t ch);
uint16_t adc_read_vrefint_raw(void);
uint16_t adc_read_vdda_mv(void);
uint16_t adc_vdda_from_raw(uint16_t raw);

#endif /* ADC_H */
