/*
 * hal.c — Hardware abstraction layer
 * Bridges radio/USB to the protocol stack
 */

#include <stddef.h>
#include <string.h>
#include "hal.h"
#include "usb_cdc.h"
#include "timer.h"

void recieveMode(void) {
    radio_start_rx();
}

GLOB_RET transmitFrame(Packet *pkt) {
    uint8_t payload[70];
    size_t size_to_copy = sizeof(Packet) - offsetof(Packet, destination_adr);
    memcpy(payload, (uint8_t *)pkt + offsetof(Packet, destination_adr), size_to_copy);

    GLOB_RET errorCode = radio_send(payload, size_to_copy);

    if (0 == errorCode) {
        if (pkt->control_app == PAYLOAD && pkt->pktDir != RETX) {
            print("Done\n");
            delay_ms(100);
        }
        errorCode = GLOB_SUCCESS;
    }
    return errorCode;
}

bool print(void *data) {
    return cdc_print_str((const char *)data);
}
