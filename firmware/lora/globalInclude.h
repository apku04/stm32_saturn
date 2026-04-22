/*
 * globalInclude.h — Shared types and defines for STM32 LoRa port
 * Ported from PIC24 pic24LoraAlpha project
 */

#ifndef GLOB_H
#define GLOB_H

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ---- Buffer / packet sizes ---- */
#define BUFFER_SIZE           8
#define PACKET_SIZE           70
#define PACKET_BUFFER_SIZE    10
#define SIGNAL_STRENGTH_THRESHOLD -100
#define BROADCAST             255

/* ---- Flash parameter addresses ---- */
typedef enum {
    DEVICE_ID = 0,
    RADIO_FREQ1,
    RADIO_FREQ2,
    RADIO_FREQ3,
    RADIO_FREQ4,
    RADIO_DR,
    RADIO_TX_PWR,
    PARAMETER_N,
    FLASH_SIZE_PARAM
} addrEnum;

/* ---- Packet direction / ownership ---- */
typedef enum { DIR_EMPTY, OUTGOING, INCOMING, RETX } Direction;
typedef enum { OWNER_EMPTY, PHY, MAC, NET, APP }     Owner;
typedef enum { BEACON, PAYLOAD, ACK, PING, PONG }    PacketType;

/* ---- Return codes ---- */
typedef enum {
    DATA_LINK_TRANSMISSION_FAILED       =  8,
    DATA_LINK_ACK_TIMEOUT               =  7,
    DATA_LINK_GOT_ACK                   =  6,
    DATA_LINK_CTS_TIMEOUT               =  5,
    DATA_LINK_GOT_CTS                   =  4,
    DATA_LINK_DIFS_WAIT_OK              =  3,
    DATA_LINK_BACKOFF_OK                =  2,
    DATA_LINK_CARRIER_DETECTED          =  1,
    GLOB_SUCCESS                        =  0,
    GLOB_FAILURE                        = -1,
    GLOB_ERROR_OUT_OF_RANGE_PARAM       = -2,
    GLOB_ERROR_INVALID_MESSAGE          = -3,
    GLOB_ERROR_INVALID_PARAM            = -4,
    GLOB_ERROR_OUT_OF_HANDLES           = -5,
    GLOB_ERROR_INIT                     = -6,
    GLOB_ERROR_NETWORK_OUTGOING_FAILED  = -7,
    GLOB_ERROR_NETWORK_INCOMING_FAILED  = -8,
    GLOB_ERROR_UNKNOWN_DIRECTION        = -9,
    GLOB_ERROR_BUFFER_FULL              = -10,
    GLOB_ERROR_BUFFER_EMPTY             = -11,
    GLOB_ERROR_WRITE_BUFFER_SIZE_EXCEEDED   = -12,
    GLOB_ERROR_WRITE_BUFFER_DATA_SIZE_EXCEEDED = -13,
    GLOB_ERROR_READ_BUFFER_SIZE_EXCEEDED    = -14,
    GLOB_ERROR_BUFFER_SIZE_EXCEEDED     = -15,
    GLOB_ERROR_BUFFER_SIZE_NEGATIVE     = -16,
    GLOB_ERROR_POINTER_MISMATCH         = -17,
    GLOB_WARNING_PARAM_NOT_SET          = -20,
    GLOB_ERROR_MEMORY_ALLOCATION        = -21,
    GLOB_ERROR_BEACON_PROCESSING_FAILED = -22
} GLOB_RET;

/* ---- Timer defines ---- */
#define TIMER_ONE_SEC   1000
#define TIMER_100_MS     100
#define TIMER_50_MS       50
#define TIMER_10_MS       10
#define TIMER_1_MS         1

#define NO  0
#define YES 1

/* ---- Device data stored in flash ---- */
typedef struct {
    uint8_t deviceID;
    uint8_t radioFreq1;
    uint8_t radioFreq2;
    uint8_t radioFreq3;
    uint8_t radioFreq4;
    uint8_t radioDataRate;
    uint8_t radioTxPwr;
    uint8_t paramN;
} deviceData_t;

/* ---- Packet structure (packed, same as PIC24) ---- */
typedef struct __attribute__((__packed__)) {
    uint8_t  pktDir;
    uint8_t  pOwner;
    int16_t  rssi;
    int16_t  prssi;
    uint8_t  rxCnt;
    uint8_t  destination_adr;   /* 0 */
    uint8_t  source_adr;        /* 1 */
    uint16_t sequence_num;      /* 2,3 */
    uint8_t  control_mac;       /* 4 */
    uint8_t  protocol_Ver;      /* 5 */
    uint8_t  TTL;               /* 6 */
    uint8_t  mesh_dest;         /* 7 */
    uint8_t  mesh_tbl_entries;  /* 8 */
    uint8_t  mesh_src;          /* 9 */
    uint8_t  control_app;       /* 10 */
    uint8_t  length;            /* 11 */
    uint8_t  data[50];
} Packet;

/* ---- Packet buffer ---- */
typedef struct {
    Packet buffer[PACKET_BUFFER_SIZE];
    int read_pointer;
    int write_pointer;
    int data_size;
} PacketBuffer;

/* ---- Header sizes ---- */
#define APP_HEADER_SIZE     4
#define NET_HEADER_SIZE     5
#define MAC_HEADER_SIZE     3
#define PACKET_HEADER_SIZE  (MAC_HEADER_SIZE + NET_HEADER_SIZE + APP_HEADER_SIZE)
#define META_DATA_SIZE      3
#define DATA_LEN            50
#define BUFFER_DATA_SIZE    (META_DATA_SIZE + PACKET_HEADER_SIZE + DATA_LEN)

#define TRUE  1
#define FALSE 0

/* ---- State machine struct ---- */
typedef struct slme {
    uint8_t radio_initialized;
    uint8_t sender_initialized;
    uint8_t receiver_initialized;
    int16_t pkt_snr;
    uint8_t pkt_rssi;
    uint8_t rssi;
} slme;

/* ---- Sniffer flag ---- */
extern uint8_t sniffer;

#endif /* GLOB_H */
