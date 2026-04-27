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

/*
 * Compute a 1-byte fingerprint from the STM32 96-bit factory UID.
 * Uses FNV-1a over all 12 bytes folded down — much better entropy
 * than a flat XOR fold, which collides on sibling dies (same lot/wafer).
 */
static uint8_t uid_fingerprint(void) {
    const volatile uint8_t *p = (const volatile uint8_t *)UID_BASE;
    uint32_t h = 0x811C9DC5u;  /* FNV-1a 32-bit offset basis */
    for (uint8_t i = 0; i < 12; i++) {
        h ^= (uint32_t)p[i];
        h *= 0x01000193u;      /* FNV-1a 32-bit prime */
    }
    /* Fold 32 -> 8 with XOR */
    uint8_t fp = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    return (fp == 0xFF) ? 0xFE : fp;   /* never collide with erased */
}

/* Derive a valid MAC address (1..253) from the UID using FNV-1a. */
static uint8_t uid_derive_address(void) {
    const volatile uint8_t *p = (const volatile uint8_t *)UID_BASE;
    /* Different seed than uid_fingerprint() so address != fingerprint
     * (otherwise a clone-detect that re-derives address could land back
     * on the same value as the stored fingerprint). */
    uint32_t h = 0xDEADBEEFu;
    for (uint8_t i = 0; i < 12; i++) {
        h ^= (uint32_t)p[i];
        h *= 0x01000193u;
    }
    uint8_t derived = (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
    if (derived == 0)   derived = 1;
    if (derived >= 254) derived = 253;
    return derived;
}

void mac_layer_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf) {
    Mlme.pktRxBuf = pRxBuf;
    Mlme.pktTxBuf = pTxBuf;
    Mlme.headerSize = MAC_HEADER_SIZE;

    uint8_t stored_addr = 0;
    uint8_t stored_fp   = 0xFF;
    readFlash(DEVICE_ID, &stored_addr);
    readFlash(UID_FINGERPRINT, &stored_fp);

    uint8_t cur_fp = uid_fingerprint();

    if (stored_addr >= 1 && stored_addr <= 254) {
        if (stored_fp == 0xFF) {
            /* First boot with new firmware on an existing board.
             * Preserve the intentionally-set address; stamp our UID
             * fingerprint so future clones can be detected. */
            writeFlashByte(UID_FINGERPRINT, cur_fp);
            Mlme.mAddr = stored_addr;
        } else if (stored_fp == cur_fp) {
            /* Same board that wrote this config — use stored address. */
            Mlme.mAddr = stored_addr;
        } else {
            /* UID fingerprint mismatch → cloned flash image.
             * Derive a unique address from this board's UID. */
            Mlme.mAddr = uid_derive_address();
        }
    } else {
        /* No valid address in flash — derive from UID (first boot). */
        Mlme.mAddr = uid_derive_address();
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
