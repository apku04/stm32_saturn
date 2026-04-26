/*
 * gps.h — u-blox NEO-6M (GY-GPS6MV2) NMEA receiver on USART2
 *
 * Wiring (per hw_pins.h):
 *   PA2  USART2_TX (AF7) → GPS_RX
 *   PA3  USART2_RX (AF7) ← GPS_TX
 *
 * Default: 9600 baud, 8N1. Polled — call gps_poll() from main loop.
 */

#ifndef GPS_H
#define GPS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     valid;        /* fix_quality > 0 */
    uint8_t  fix_quality;  /* 0=no fix, 1=GPS, 2=DGPS */
    uint8_t  sats;
    int32_t  lat_udeg;     /* signed micro-degrees (1e-6 deg) */
    int32_t  lon_udeg;
    int16_t  alt_m;        /* meters above MSL */
    uint32_t time_hms;     /* hhmmss */
    uint32_t sentences;    /* total NMEA lines received */
} gps_fix_t;

void              gps_init(void);
void              gps_poll(void);
const gps_fix_t * gps_get_fix(void);
/* Copy last received NMEA line (NUL-terminated) into out. Returns length. */
uint16_t          gps_get_last_nmea(char *out, uint16_t maxlen);

/* Diagnostics: returns USART2_ISR; out_pa3_toggles = number of PA3 input
 * level changes observed over ~50 ms window. */
uint32_t          gps_diag(uint32_t *out_pa3_toggles);

/* Enable/disable raw byte echo to CDC for debugging. */
void              gps_set_raw_echo(uint8_t on);

/* Reconfigure UART baud rate at runtime. */
void              gps_set_baud(uint32_t baud);

/* Reconfigure PA2/PA3 alternate function nibble (0-15) at runtime. */
void              gps_set_af(uint8_t af);

/* Probe PA3 as plain GPIO input for ~50ms; returns:
 *   bits  0..15 : transition count (saturated at 0xFFFF)
 *   bits 16..31 : "low samples" count (out of 50000)
 * After return, PA3 is left as GPIO input (USART2 disabled).
 * Call gps_set_af(...) afterwards to restore UART operation. */
uint32_t          gps_probe_pa3(void);

#endif /* GPS_H */
