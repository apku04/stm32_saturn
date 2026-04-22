/*
 * maclayer.h — MAC layer (ALOHA channel access)
 */

#ifndef MACLAYER_H
#define MACLAYER_H

#include "globalInclude.h"
#include "radio.h"
#include "hal.h"
#include "networklayer.h"
#include "packetBuffer.h"
#include "timer.h"

typedef struct {
    PacketBuffer *pktRxBuf;
    PacketBuffer *pktTxBuf;
    char headerSize;
    char mBcast;
    char mAddr;
    char macFrameSize;
} mlme;

void     mac_layer_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf);
GLOB_RET mac_interface(char iKey);
GLOB_RET set_mac_address(uint8_t addr);
uint8_t  get_mac_address(void);

#endif /* MACLAYER_H */
