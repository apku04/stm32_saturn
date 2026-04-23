/*
 * maclayer.c — ALOHA-based MAC layer
 * Ported from PIC24 — nearly identical logic
 */

#include <stdlib.h>
#include <string.h>
#include "maclayer.h"
#include "flash_config.h"
#include "stm32u0.h"

static GLOB_RET mac_outgoing(Packet *pkt);
static GLOB_RET mac_incoming(Packet *pkt);
static void aloha_channel_access(Packet *pkt);

mlme Mlme;

static void aloha_channel_access(Packet *pkt) {
    uint16_t di = (rand() % 10 + 2);
    delay_ms(di * 20);

    uint8_t isChannelBusy = radio_get_carrier_detect_avg();

    while (isChannelBusy >= 1) {
        uint16_t d = (rand() % 10 + 2);
        delay_ms(d * 200);
        isChannelBusy = radio_get_carrier_detect_avg();
    }

    transmitFrame(pkt);
}

static GLOB_RET mac_incoming(Packet *pkt) {
    if (pkt->source_adr != Mlme.mAddr) {
        if (BROADCAST == pkt->destination_adr ||
            Mlme.mAddr == pkt->destination_adr ||
            0 == pkt->destination_adr) {
            pkt->pOwner = NET;
            pkt->pktDir = INCOMING;
            if (GLOB_ERROR_BUFFER_FULL != buffer_full(Mlme.pktRxBuf))
                write_packet(Mlme.pktRxBuf, pkt);
        }
    }
    return GLOB_SUCCESS;
}

static GLOB_RET mac_outgoing(Packet *pkt) {
    if (pkt->pktDir == OUTGOING) {
        pkt->length = pkt->length + Mlme.headerSize;
        pkt->source_adr = Mlme.mAddr;
        pkt->control_mac = 0;
        pkt->protocol_Ver = 10;
    }

    if (255 == pkt->destination_adr)
        Mlme.mBcast = YES;
    else
        Mlme.mBcast = NO;

    aloha_channel_access(pkt);
    return GLOB_SUCCESS;
}

GLOB_RET set_mac_address(uint8_t addr) {
    if (addr < 1 || addr > 254)
        return GLOB_ERROR_OUT_OF_RANGE_PARAM;
    Mlme.mAddr = addr;
    return GLOB_SUCCESS;
}

uint8_t get_mac_address(void) {
    return Mlme.mAddr;
}

void mac_layer_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf) {
    Mlme.pktRxBuf = pRxBuf;
    Mlme.pktTxBuf = pTxBuf;
    Mlme.headerSize = MAC_HEADER_SIZE;

    uint8_t stored_addr = 0;
    readFlash(DEVICE_ID, &stored_addr);
    if (stored_addr >= 1 && stored_addr <= 254) {
        Mlme.mAddr = stored_addr;
    } else {
        /* Derive a unique address from the 96-bit factory UID */
        uint32_t uid = (*(volatile uint32_t *)(UID_BASE))
                     ^ (*(volatile uint32_t *)(UID_BASE + 4))
                     ^ (*(volatile uint32_t *)(UID_BASE + 8));
        uint8_t derived = (uint8_t)((uid ^ (uid >> 8) ^ (uid >> 16) ^ (uid >> 24)) & 0xFF);
        if (derived == 0 || derived >= 254)
            derived = (derived ^ 0x55) & 0xFE;
        if (derived == 0)
            derived = 1;
        Mlme.mAddr = derived;
    }

    srand(4125);
}

GLOB_RET mac_interface(char iKey) {
    GLOB_RET ret = GLOB_SUCCESS;

    if (OUTGOING == iKey || RETX == iKey) {
        Packet pkt = search_pending_packet(Mlme.pktTxBuf, iKey, MAC);
        if (pkt.pktDir != DIR_EMPTY && pkt.pOwner != OWNER_EMPTY)
            ret = mac_outgoing(&pkt);
    } else if (INCOMING == iKey) {
        Packet pkt = search_pending_packet(Mlme.pktRxBuf, iKey, MAC);
        if (pkt.pktDir != DIR_EMPTY && pkt.pOwner != OWNER_EMPTY)
            ret = mac_incoming(&pkt);
    } else {
        ret = GLOB_ERROR_INVALID_PARAM;
    }
    return ret;
}
