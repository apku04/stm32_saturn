/*
 * packetBuffer.h — Circular packet buffer
 */

#ifndef PACKETBUFF_H
#define PACKETBUFF_H

#include "globalInclude.h"

GLOB_RET init_packet_buffer(PacketBuffer *pBuf);
GLOB_RET buffer_full(PacketBuffer *pBuf);
GLOB_RET buffer_empty(PacketBuffer *pBuf);
GLOB_RET write_packet(PacketBuffer *pBuf, Packet *packet);
GLOB_RET read_packet(PacketBuffer *pBuf, Packet *packet);
GLOB_RET search_packet_buffer(PacketBuffer *pBuf, uint8_t source_adr, uint32_t sequence_num);
Packet   search_pending_packet(PacketBuffer *pBuf, Direction dir, Owner me);
bool     remove_packet_from_buffer(PacketBuffer *pBuf, Packet *pkt);

#endif /* PACKETBUFF_H */
