/*
 * terminal.h — USB terminal/command interface
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include "globalInclude.h"

typedef struct {
    PacketBuffer *pktRxBuf;
    PacketBuffer *pktTxBuf;
} ulme;

void terminal(uint8_t *msg, uint8_t size);
void user_layer_init(PacketBuffer *pTxBuf);
void print_routing_table(void);

#endif /* TERMINAL_H */
