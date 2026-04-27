/*
 * flash_config.h — Flash parameter storage for STM32U073
 */

#ifndef FLASH_CONFIG_H
#define FLASH_CONFIG_H

#include "globalInclude.h"

void writeFlash(deviceData_t *data);
void readFlash(addrEnum addr, uint8_t *read_data);
void writeFlashByte(addrEnum addr, uint8_t value);

#endif /* FLASH_CONFIG_H */
