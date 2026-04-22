/*
 * networklayer.h — Network layer (mesh routing, beacons)
 */

#ifndef NETWORKLAYER_H
#define NETWORKLAYER_H

#include "globalInclude.h"

#define MAX_RX_HIST_CNT    10
#define MAX_ENTRIES        10
#define MAX_HOPS           3
#define MAX_DEST_ADDR      254
#define ROUTE_TIMEOUT_COUNT 10

typedef struct { uint16_t seq; uint8_t adr; } rxHistory;
typedef struct {
    uint8_t destination;
    uint8_t next_hop;
    uint8_t hop_count;
    uint8_t age;
} RoutingEntry;

typedef struct {
    PacketBuffer *pktRxBuf;
    PacketBuffer *pktTxBuf;
    char ttl;
    uint8_t mesh_dest;
    char headerSize;
    char networkSize;
    rxHistory rxHist[MAX_RX_HIST_CNT];
    uint8_t rxHistCnt;
    RoutingEntry routing_table[MAX_ENTRIES];
    uint8_t num_entries;
} nlme;

GLOB_RET       network_interface(Direction iKey);
void           network_layer_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf);
RoutingEntry*  get_routing_table(void);
uint8_t        get_routing_entries(void);
uint8_t        calculate_crc8(uint8_t *data, uint8_t length);

#endif /* NETWORKLAYER_H */
