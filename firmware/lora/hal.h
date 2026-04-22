/*
 * hal.h — Hardware abstraction layer
 */

#ifndef HAL_H
#define HAL_H

#include "globalInclude.h"
#include "radio.h"

void     recieveMode(void);
GLOB_RET transmitFrame(Packet *pkt);
bool     print(void *data);

/* Called from main to signal beacon enable/disable */
void     send_beacon_flag(uint8_t flag);

#endif /* HAL_H */
