#ifndef ADC_H
#define ADC_H

#include <stdint.h>

void     adc_init(void);
uint16_t adc_read_battery_raw(void);
uint16_t adc_read_battery_mv(void);
uint16_t adc_read_channel_raw(uint8_t ch);

#endif /* ADC_H */
