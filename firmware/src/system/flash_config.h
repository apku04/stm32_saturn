/*
 * flash_config.h — Flash parameter storage for STM32U073
 */

#ifndef FLASH_CONFIG_H
#define FLASH_CONFIG_H

#include "globalInclude.h"

void writeFlash(deviceData_t *data);
void readFlash(addrEnum addr, uint8_t *read_data);
void writeFlashByte(addrEnum addr, uint8_t value);
void writeFlashParamByte(addrEnum addr, uint8_t value);

/* Write several parameter slots in one page erase+rewrite pass — used where
 * a set of fields must land together (e.g. a config-OTA commit: new values +
 * rollback backup + pending flag), rather than N separate erase cycles. */
void writeFlashParamBytes(const addrEnum *addrs, const uint8_t *vals, uint8_t n);

#endif /* FLASH_CONFIG_H */
