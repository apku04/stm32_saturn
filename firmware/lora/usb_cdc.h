/*
 * usb_cdc.h — USB CDC (Virtual COM Port) driver for STM32U073
 */

#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>
#include <stdbool.h>

void usb_cdc_init(void);
void usb_cdc_poll(void);
bool usb_cdc_is_configured(void);
bool cdc_print_str(const char *str);

/* Callback for received CDC data — set by application */
typedef void (*cdc_rx_callback_t)(uint8_t *data, uint16_t len);
void usb_cdc_set_rx_callback(cdc_rx_callback_t cb);

#endif /* USB_CDC_H */
