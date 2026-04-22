/*
 * radio.h — Radio HAL interface (same API as PIC24 version)
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>
#include "globalInclude.h"

uint8_t  radio_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf);
uint8_t  radio_start_rx(void);
uint8_t  radio_start_tx(void);
void     radio_irq_handler(void);
uint8_t  radio_send(uint8_t *payload, uint8_t length);
uint8_t  radio_set_channel(uint32_t freq_hz);
uint32_t radio_get_channel(void);
void     radio_set_datarate(uint8_t sf);
uint8_t  radio_get_datarate(void);
void     radio_set_tx_power(uint8_t power);
uint8_t  radio_get_tx_power(void);
uint8_t  radio_get_carrier_detect_avg(void);
void     radio_print_all_registers(void);

#endif /* RADIO_H */
