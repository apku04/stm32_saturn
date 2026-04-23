/*
 * networklayer.c — Mesh network layer with routing table and beacons
 * Ported from PIC24 — logic unchanged
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "networklayer.h"
#include "maclayer.h"
#include "packetBuffer.h"
#include "hal.h"

static GLOB_RET network_outgoing(Packet *pkt);
static GLOB_RET network_incoming(Packet *pkt);
static GLOB_RET handle_broadcast_packet(Packet *pkt);
static GLOB_RET handle_regular_packet(Packet *pkt);
static bool process_received_beacon(Packet *pkt);
static bool is_valid_address(uint8_t address);
static void age_routing_table(void);
static void remove_routing_entry(int index);
static char find_next_hop(void);

nlme Nlme;

#define CRC8_POLYNOMIAL 0x07

uint8_t verify_crc8(uint8_t *data, uint8_t length, uint8_t crc_received) {
    return (calculate_crc8(data, length) == crc_received) ? 1 : 0;
}

uint8_t calculate_crc8(uint8_t *data, uint8_t length) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ CRC8_POLYNOMIAL;
            else            crc <<= 1;
        }
    }
    return crc;
}

static bool is_valid_address(uint8_t address) {
    return address >= 1 && address <= MAX_DEST_ADDR;
}

static void remove_routing_entry(int index) {
    if (index < 0 || index >= Nlme.num_entries) return;
    for (int i = index; i < Nlme.num_entries - 1; i++)
        Nlme.routing_table[i] = Nlme.routing_table[i + 1];
    memset(&Nlme.routing_table[Nlme.num_entries - 1], 0, sizeof(RoutingEntry));
    Nlme.num_entries--;
}

static void age_routing_table(void) {
    int i = 0;
    while (i < Nlme.num_entries) {
        Nlme.routing_table[i].age++;
        if (Nlme.routing_table[i].age > ROUTE_TIMEOUT_COUNT)
            remove_routing_entry(i);
        else
            i++;
    }
}

static GLOB_RET handle_broadcast_packet(Packet *pkt) {
    for (int i = 0; i < MAX_RX_HIST_CNT; i++) {
        if (Nlme.rxHist[i].adr == pkt->source_adr &&
            Nlme.rxHist[i].seq == pkt->sequence_num)
            return GLOB_SUCCESS;
    }

    if (GLOB_ERROR_BUFFER_FULL != buffer_full(Nlme.pktTxBuf)) {
        GLOB_RET ret = search_packet_buffer(Nlme.pktTxBuf, pkt->source_adr, pkt->sequence_num);
        if (GLOB_FAILURE == ret) {
            pkt->pOwner = MAC;
            pkt->pktDir = RETX;
            pkt->TTL = pkt->TTL - 1;
            write_packet(Nlme.pktTxBuf, pkt);
            Nlme.rxHist[Nlme.rxHistCnt].adr = pkt->source_adr;
            Nlme.rxHist[Nlme.rxHistCnt].seq = pkt->sequence_num;
            Nlme.rxHistCnt = (Nlme.rxHistCnt + 1) % MAX_RX_HIST_CNT;
        }

        pkt->pOwner = APP;
        pkt->pktDir = INCOMING;
        write_packet(Nlme.pktRxBuf, pkt);
    }
    return GLOB_SUCCESS;
}

static GLOB_RET handle_regular_packet(Packet *pkt) {
    if (GLOB_ERROR_BUFFER_FULL != buffer_full(Nlme.pktRxBuf)) {
        pkt->pOwner = APP;
        pkt->pktDir = INCOMING;
        write_packet(Nlme.pktRxBuf, pkt);
    }
    return GLOB_SUCCESS;
}

static bool process_received_beacon(Packet *pkt) {
    uint8_t max_possible_entries = (sizeof(pkt->data) - 1) / 3;
    if (pkt->mesh_tbl_entries > max_possible_entries) return false;

    uint8_t table_length = pkt->length - PACKET_HEADER_SIZE;
    if (pkt->mesh_tbl_entries > 0 && table_length > 0) {
        if (table_length > sizeof(pkt->data)) return false;
        if (!verify_crc8(pkt->data, table_length - 1, pkt->data[table_length - 1]))
            return false;
    }

    uint8_t source_address = pkt->source_adr;
    int8_t signal_strength = pkt->prssi;
    if (signal_strength < SIGNAL_STRENGTH_THRESHOLD) return false;

    uint8_t my_addr = get_mac_address();
    if (source_address == my_addr) return false;

    /* Update direct route */
    int found_index = -1;
    for (int i = 0; i < Nlme.num_entries; i++) {
        if (Nlme.routing_table[i].destination == source_address) {
            found_index = i;
            break;
        }
    }
    if (found_index == -1 && Nlme.num_entries < MAX_ENTRIES) {
        Nlme.routing_table[Nlme.num_entries] = (RoutingEntry){ source_address, source_address, 1, 0 };
        Nlme.num_entries++;
    } else if (found_index != -1) {
        Nlme.routing_table[found_index].age = 0;
        Nlme.routing_table[found_index].hop_count = 1;
        Nlme.routing_table[found_index].next_hop = source_address;
    }

    /* Process routing entries from beacon */
    uint8_t entries_to_process = pkt->mesh_tbl_entries;
    if (entries_to_process > MAX_ENTRIES) entries_to_process = MAX_ENTRIES;

    for (int i = 0; i < entries_to_process; i++) {
        uint8_t data_index = i * 3;
        if ((data_index + 2) >= sizeof(pkt->data)) break;

        RoutingEntry entry = {
            .destination = pkt->data[data_index],
            .next_hop = pkt->data[data_index + 1],
            .hop_count = pkt->data[data_index + 2]
        };

        if (entry.destination == 0 && entry.next_hop == 0 && entry.hop_count == 0)
            continue;
        if (!is_valid_address(entry.destination) || !is_valid_address(entry.next_hop))
            continue;
        if (entry.destination == my_addr) continue;

        int found = -1;
        for (int j = 0; j < Nlme.num_entries; j++) {
            if (Nlme.routing_table[j].destination == entry.destination) {
                found = j;
                break;
            }
        }

        if (found != -1) {
            if (entry.hop_count + 1 < Nlme.routing_table[found].hop_count) {
                Nlme.routing_table[found].hop_count = entry.hop_count + 1;
                Nlme.routing_table[found].next_hop = source_address;
            }
        } else if (Nlme.num_entries < MAX_ENTRIES && entry.hop_count == 1) {
            Nlme.routing_table[Nlme.num_entries] = (RoutingEntry){
                entry.destination, source_address, entry.hop_count + 1, 1
            };
            Nlme.num_entries++;
        }
    }

    pkt->pOwner = APP;
    pkt->pktDir = INCOMING;
    /* Only strip CRC byte when routing table was present (CRC is appended
       by network_outgoing only when mesh_tbl_entries > 0). */
    if (pkt->mesh_tbl_entries > 0)
        pkt->length = pkt->length - 1;
    write_packet(Nlme.pktRxBuf, pkt);
    return true;
}

static GLOB_RET network_outgoing(Packet *pkt) {
    pkt->pOwner = MAC;
    pkt->pktDir = OUTGOING;
    pkt->TTL = Nlme.ttl;
    pkt->mesh_dest = Nlme.mesh_dest;
    pkt->mesh_src = get_mac_address();

    if (pkt->control_app == BEACON || pkt->control_app == PING || pkt->control_app == PONG) {
        if (pkt->control_app == PING || pkt->control_app == PONG)
            pkt->TTL = 0;
        if (pkt->control_app == BEACON)
            age_routing_table();

        /* Beacons carry telemetry at data[0]; don't prepend routing table
           (avoids offset mismatch on the RX decoder). PING/PONG still
           carry the table for mesh discovery. */
        pkt->mesh_tbl_entries = (pkt->control_app == BEACON) ? 0 : Nlme.num_entries;
        pkt->destination_adr = BROADCAST;

        if (pkt->mesh_tbl_entries > 0) {
            uint8_t app_data_len = (pkt->length > APP_HEADER_SIZE) ? (pkt->length - APP_HEADER_SIZE) : 0;
            uint8_t app_backup[50];
            if (app_data_len > 0 && app_data_len <= sizeof(app_backup))
                memcpy(app_backup, pkt->data, app_data_len);

            memset(pkt->data, 0, 50);

            for (int i = 0; i < Nlme.num_entries; i++) {
                pkt->data[i * 3]     = Nlme.routing_table[i].destination;
                pkt->data[i * 3 + 1] = Nlme.routing_table[i].next_hop;
                pkt->data[i * 3 + 2] = Nlme.routing_table[i].hop_count;
            }
            uint8_t table_length = Nlme.num_entries * 3;

            if (app_data_len > 0 && app_data_len <= sizeof(app_backup))
                memcpy(&pkt->data[table_length], app_backup, app_data_len);

            uint8_t total_payload = table_length + app_data_len;
            pkt->length += table_length;
            uint8_t crc = calculate_crc8(pkt->data, total_payload);
            pkt->data[total_payload] = crc;
            pkt->length += 1;
        }
        pkt->length += Nlme.headerSize;
    } else {
        pkt->destination_adr = find_next_hop();
        pkt->length += Nlme.headerSize;
    }

    if (GLOB_ERROR_BUFFER_FULL != buffer_full(Nlme.pktTxBuf))
        write_packet(Nlme.pktTxBuf, pkt);

    return GLOB_SUCCESS;
}

static GLOB_RET network_incoming(Packet *pkt) {
    if (pkt->control_app == BEACON) {
        if (!process_received_beacon(pkt))
            return GLOB_ERROR_BEACON_PROCESSING_FAILED;
    } else if (pkt->destination_adr == BROADCAST && pkt->TTL > 0) {
        return handle_broadcast_packet(pkt);
    } else {
        return handle_regular_packet(pkt);
    }
    return GLOB_SUCCESS;
}

static char find_next_hop(void) {
    return BROADCAST;
}

void network_layer_init(PacketBuffer *pRxBuf, PacketBuffer *pTxBuf) {
    Nlme.pktRxBuf = pRxBuf;
    Nlme.pktTxBuf = pTxBuf;
    Nlme.ttl = 7;
    Nlme.headerSize = NET_HEADER_SIZE;
    Nlme.networkSize = 20;
    Nlme.num_entries = 0;
    memset(Nlme.routing_table, 0, sizeof(RoutingEntry) * MAX_ENTRIES);
}

GLOB_RET network_interface(Direction iKey) {
    if (OUTGOING == iKey) {
        Packet pkt = search_pending_packet(Nlme.pktTxBuf, iKey, NET);
        if (pkt.pktDir != DIR_EMPTY && pkt.pOwner != OWNER_EMPTY)
            return network_outgoing(&pkt);
    } else if (INCOMING == iKey) {
        Packet pkt = search_pending_packet(Nlme.pktRxBuf, iKey, NET);
        if (pkt.pktDir != DIR_EMPTY && pkt.pOwner != OWNER_EMPTY)
            return network_incoming(&pkt);
    }
    return GLOB_SUCCESS;
}

RoutingEntry* get_routing_table(void) { return Nlme.routing_table; }
uint8_t get_routing_entries(void) { return Nlme.num_entries; }
