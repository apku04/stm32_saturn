/*
 * gps.c — u-blox NEO-6M NMEA receiver on USART2 @ 9600 8N1
 *
 * Polled: call gps_poll() from main loop. Drains USART2 RX, assembles
 * lines, parses $GPGGA into a gps_fix_t.
 */

#include "gps.h"
#include "stm32u0.h"
#include "hw_pins.h"
#include "hal.h"
#include "timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* USART2 registers and bits now in stm32u0.h */

/* USART2 IRQ number on STM32U073 (RM0503 Table 62) */
#define USART2_IRQn  28u

/* ---- Ring buffer for ISR-captured USART2 bytes ---- */
#define RX_BUF_SIZE  64u
static volatile uint8_t rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_head;   /* ISR writes here */
static volatile uint8_t rx_tail;   /* gps_poll reads here */
static volatile uint32_t fe_count; /* framing errors seen by ISR */
static volatile uint32_t isr_calls; /* total ISR invocations */
static volatile uint32_t rxne_count; /* total RXNE events */

#define LINE_MAX 96

static char     line_buf[LINE_MAX];
static uint16_t line_len;
static char     last_nmea[LINE_MAX];
static uint16_t last_nmea_len;
static gps_fix_t fix;
static uint8_t  raw_echo = 0;   /* if 1, print every byte to CDC */
static uint32_t init_tick;       /* tick_ms at last gps_init/baud change */
static uint8_t  baud_switched;   /* 1 = already tried 4800 fallback */

void gps_set_raw_echo(uint8_t on) { raw_echo = on ? 1 : 0; }

void gps_set_baud(uint32_t baud) {
    if (baud == 0) return;
    USART2_CR1 = 0;                          /* disable to change BRR */
    USART2_BRR = 16000000u / baud;
    USART2_CR1 = USART_CR1_RXNEIE | USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
    line_len = 0;
    init_tick = get_tick_ms();
}

void gps_set_af(uint8_t af) {
    af &= 0xFu;
    USART2_CR1 = 0;
    uint32_t a = GPIO_AFRL(GPIOA_BASE);
    a &= ~((0xFu << (2 * 4)) | (0xFu << (3 * 4)));
    a |=  (((uint32_t)af << (2 * 4)) | ((uint32_t)af << (3 * 4)));
    GPIO_AFRL(GPIOA_BASE) = a;
    USART2_CR1 = USART_CR1_RXNEIE | USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
    line_len = 0;
}

uint32_t gps_probe_pa3(void) {
    /* Disable USART2, switch PA3 to plain GPIO input, no pull. */
    USART2_CR1 = 0;
    uint32_t m = GPIO_MODER(GPIOA_BASE);
    m &= ~(3u << (3 * 2));            /* 00 = input */
    GPIO_MODER(GPIOA_BASE) = m;
    uint32_t pu = GPIO_PUPDR(GPIOA_BASE);
    pu &= ~(3u << (3 * 2));           /* 00 = no pull */
    GPIO_PUPDR(GPIOA_BASE) = pu;

    uint32_t low_count = 0;
    uint32_t transitions = 0;
    uint32_t last = (GPIO_IDR(GPIOA_BASE) >> 3) & 1u;
    /* ~1,000,000 samples × ~1µs each ≈ 1 second; long enough to catch
     * at least one NMEA burst from a 1Hz GPS module. */
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint32_t cur = (GPIO_IDR(GPIOA_BASE) >> 3) & 1u;
        if (cur == 0) low_count++;
        if (cur != last) {
            if (transitions < 0xFFFFu) transitions++;
            last = cur;
        }
    }
    /* Pack: transitions in low 16 bits, low_count clamped to 16-bit upper */
    if (low_count > 0xFFFFu) low_count = 0xFFFFu;
    uint32_t result = (low_count << 16) | (transitions & 0xFFFFu);

    /* Restore PA3 to AF mode (USART2_RX) */
    uint32_t m2 = GPIO_MODER(GPIOA_BASE);
    m2 &= ~(3u << (3 * 2));
    m2 |=  (2u << (3 * 2));   /* AF mode */
    GPIO_MODER(GPIOA_BASE) = m2;
    uint32_t pu2 = GPIO_PUPDR(GPIOA_BASE);
    pu2 &= ~(3u << (3 * 2));
    pu2 |=  (1u << (3 * 2));  /* pull-up */
    GPIO_PUPDR(GPIOA_BASE) = pu2;
    /* Re-enable USART2 with RXNE interrupt */
    USART2_CR1 = USART_CR1_RXNEIE | USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;

    return result;
}

void gps_init(void) {
    /* GPIOA clock (PA2/PA3/PA15) */
    RCC_IOPENR |= (1u << 0);

    /* --- Fix 1: Power the GPS module via SENSE_LDO_EN (PA15) --- */
    {
        uint32_t m = GPIO_MODER(GPIOA_BASE);
        m &= ~(3u << (SENSE_LDO_EN_PIN * 2));
        m |=  (1u << (SENSE_LDO_EN_PIN * 2));   /* 01 = output */
        GPIO_MODER(GPIOA_BASE) = m;
    }
    GPIO_BSRR(GPIOA_BASE) = (1u << SENSE_LDO_EN_PIN);  /* PA15 HIGH — LDO on */
    delay_ms(100);  /* GPS module power-up stabilisation */

    /* PA2/PA3 → AF mode, AF7 (USART2) — RM0503 Table 20 */
    uint32_t m = GPIO_MODER(GPIOA_BASE);
    m &= ~((3u << (GPS_RX_PIN * 2)) | (3u << (GPS_TX_PIN * 2)));
    m |=  ((2u << (GPS_RX_PIN * 2)) | (2u << (GPS_TX_PIN * 2)));
    GPIO_MODER(GPIOA_BASE) = m;

    uint32_t af = GPIO_AFRL(GPIOA_BASE);
    af &= ~((0xFu << (GPS_RX_PIN * 4)) | (0xFu << (GPS_TX_PIN * 4)));
    af |=  ((7u   << (GPS_RX_PIN * 4)) | (7u   << (GPS_TX_PIN * 4)));   /* AF7 = USART2 */
    GPIO_AFRL(GPIOA_BASE) = af;
    /* Enable pull-ups on PA2/PA3 — UART idles high; helps detect wiring */
    uint32_t pu = GPIO_PUPDR(GPIOA_BASE);
    pu &= ~((3u << (GPS_RX_PIN * 2)) | (3u << (GPS_TX_PIN * 2)));
    pu |=  ((1u << (GPS_RX_PIN * 2)) | (1u << (GPS_TX_PIN * 2)));   /* 01 = pull-up */
    GPIO_PUPDR(GPIOA_BASE) = pu;
    /* USART2 peripheral clock */
    RCC_APBENR1 |= RCC_APBENR1_USART2EN;
    for (volatile int i = 0; i < 16; i++) __asm__ volatile ("nop");

    /* Configure: disable, set BRR for 9600 @ 16 MHz PCLK1, enable RX/TX.
     * No SWAP — board net GPS_TX (GPS module's TX output) goes to PA3,
     * which is USART2_RX in the default pin mapping. */
    USART2_CR1 = 0;
    USART2_CR2 = 0;
    USART2_CR3 = 0;
    USART2_BRR = 16000000u / 9600u;        /* 9600 8N1 — NEO-6M default */

    /* Enable RXNE interrupt for ring buffer capture */
    USART2_CR1 = USART_CR1_RXNEIE | USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
    NVIC_ISER = (1u << USART2_IRQn);       /* Enable USART2 in NVIC */

    rx_head = 0;
    rx_tail = 0;
    fe_count = 0;
    isr_calls = 0;
    rxne_count = 0;
    line_len = 0;
    last_nmea_len = 0;
    last_nmea[0] = 0;
    memset(&fix, 0, sizeof fix);
    init_tick = get_tick_ms();
    baud_switched = 0;
}

/* "ddmm.mmmm" (lat) or "dddmm.mmmm" (lon) → signed micro-degrees */
static int32_t parse_latlon(const char *s, char hemi, int deg_digits) {
    if (!s || !*s) return 0;
    const char *dot = strchr(s, '.');
    int int_len = dot ? (int)(dot - s) : (int)strlen(s);
    if (int_len < deg_digits + 1) return 0;

    /* Degrees: first deg_digits chars */
    int deg = 0;
    for (int i = 0; i < deg_digits; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        deg = deg * 10 + (s[i] - '0');
    }

    /* Whole minutes: remaining int_len - deg_digits chars (1 or 2) */
    int min_int = 0;
    for (int i = deg_digits; i < int_len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        min_int = min_int * 10 + (s[i] - '0');
    }

    /* Fractional minutes: up to 6 digits after the dot */
    int frac = 0, frac_scale = 1;
    if (dot) {
        const char *p = dot + 1;
        for (int i = 0; i < 6 && p[i] >= '0' && p[i] <= '9'; i++) {
            frac = frac * 10 + (p[i] - '0');
            frac_scale *= 10;
        }
    }
    /* minutes_e6 = min_int * 1e6 + frac * (1e6 / frac_scale) */
    int32_t minutes_e6 = (int32_t)min_int * 1000000
                       + (int32_t)frac * (1000000 / (frac_scale ? frac_scale : 1));
    /* udeg = deg * 1e6 + minutes_e6 / 60 */
    int32_t udeg = (int32_t)deg * 1000000 + minutes_e6 / 60;
    if (hemi == 'S' || hemi == 'W') udeg = -udeg;
    return udeg;
}

static void parse_gga(char *s) {
    /* $GPGGA,time,lat,N/S,lon,E/W,fix,sats,hdop,alt,M,...*csum */
    char *fields[12] = {0};
    int n = 0;
    char *p = s;
    fields[n++] = p;
    while (*p && n < 12) {
        if (*p == ',') { *p = 0; fields[n++] = p + 1; }
        else if (*p == '*') { *p = 0; break; }
        p++;
    }
    if (n < 10) return;

    fix.time_hms    = (uint32_t)strtoul(fields[1], NULL, 10);
    fix.lat_udeg    = parse_latlon(fields[2], fields[3][0], 2);
    fix.lon_udeg    = parse_latlon(fields[4], fields[5][0], 3);
    fix.fix_quality = (uint8_t)atoi(fields[6]);
    fix.sats        = (uint8_t)atoi(fields[7]);
    fix.alt_m       = (int16_t)atoi(fields[9]);
    fix.valid       = (fix.fix_quality > 0);
}

/* ---- USART2 ISR: capture bytes into ring buffer ---- */
void USART2_IRQHandler(void) {
    uint32_t isr = USART2_ISR;
    isr_calls++;
    /* Clear error flags: overrun, framing error, noise error */
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        USART2_ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        if (isr & USART_ISR_FE) fe_count++;
    }
    if (isr & USART_ISR_RXNE) {
        rxne_count++;
        uint8_t byte = (uint8_t)(USART2_RDR & 0xFFu);
        uint8_t next = (rx_head + 1u) & (RX_BUF_SIZE - 1u);
        if (next != rx_tail) {              /* drop if full */
            rx_buf[rx_head] = byte;
            rx_head = next;
        }
    }
}

void gps_poll(void) {
    /* Polling fallback: drain all available USART2 bytes directly */
    for (;;) {
        uint32_t sr = USART2_ISR;
        if (sr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
            USART2_ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
            if (sr & USART_ISR_FE) fe_count++;
        }
        if (!(sr & USART_ISR_RXNE)) break;
        uint8_t byte = (uint8_t)(USART2_RDR & 0xFFu);
        uint8_t next = (rx_head + 1u) & (RX_BUF_SIZE - 1u);
        if (next != rx_tail) {
            rx_buf[rx_head] = byte;
            rx_head = next;
        }
        rxne_count++;
    }

    /* Drain ring buffer — non-blocking, returns immediately if empty */
    while (rx_tail != rx_head) {
        char c = (char)rx_buf[rx_tail];
        rx_tail = (rx_tail + 1u) & (RX_BUF_SIZE - 1u);

        if (raw_echo) {
            char hx[6];
            snprintf(hx, sizeof(hx), "%02X ", (uint8_t)c);
            print(hx);
        }

        if (c == '\r') continue;
        if (c == '\n') {
            if (line_len > 0 && line_len < LINE_MAX) {
                line_buf[line_len] = 0;
                /* save raw line */
                memcpy(last_nmea, line_buf, line_len + 1);
                last_nmea_len = line_len;
                fix.sentences++;
                /* parse known sentences (parser is destructive) */
                if (line_len >= 6 && memcmp(line_buf, "$GPGGA", 6) == 0) {
                    parse_gga(line_buf);
                } else if (line_len >= 6 && memcmp(line_buf, "$GNGGA", 6) == 0) {
                    parse_gga(line_buf);
                }
            }
            line_len = 0;
        } else if (line_len < LINE_MAX - 1) {
            line_buf[line_len++] = c;
        } else {
            /* overflow — drop line */
            line_len = 0;
        }
    }

    /* Auto-baud recovery: if no valid sentences after 3s and FE errors
     * accumulating, GPS is transmitting at wrong baud — try 4800. */
    if (!baud_switched && fix.sentences == 0 && fe_count > 10) {
        uint32_t elapsed = get_tick_ms() - init_tick;
        if (elapsed >= 3000u) {
            baud_switched = 1;
            fe_count = 0;
            rx_head = 0;
            rx_tail = 0;
            line_len = 0;
            USART2_CR1 = 0;
            USART2_BRR = 16000000u / 4800u;
            USART2_CR1 = USART_CR1_RXNEIE | USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
        }
    }
}

const gps_fix_t *gps_get_fix(void) { return &fix; }

uint16_t gps_get_last_nmea(char *out, uint16_t maxlen) {
    if (maxlen == 0) return 0;
    uint16_t n = last_nmea_len;
    if (n >= maxlen) n = maxlen - 1;
    memcpy(out, last_nmea, n);
    out[n] = 0;
    return n;
}

uint32_t gps_diag(uint32_t *out_pa3_toggles) {
    /* Cheap diag: returns ISR; out_pa3_toggles is packed:
     *   bits  0..1  : PA2/PA3 input level
     *   bits  4..7  : MODER for PA2 (2bit) and PA3 (2bit)
     *   bits  8..15 : AFRL nibble for PA2 + PA3
     *   bits 16..17 : PUPDR for PA2/PA3 lower bits
     */
    uint32_t isr   = USART2_ISR;
    uint32_t idr   = GPIO_IDR(GPIOA_BASE);
    uint32_t moder = GPIO_MODER(GPIOA_BASE);
    uint32_t afrl  = GPIO_AFRL(GPIOA_BASE);
    uint32_t pupdr = GPIO_PUPDR(GPIOA_BASE);

    uint32_t pa2_lvl = (idr >> 2) & 1u;
    uint32_t pa3_lvl = (idr >> 3) & 1u;
    uint32_t pa2_mod = (moder >> 4) & 3u;
    uint32_t pa3_mod = (moder >> 6) & 3u;
    uint32_t pa2_af  = (afrl  >> 8) & 0xFu;
    uint32_t pa3_af  = (afrl  >> 12) & 0xFu;
    uint32_t pa2_pu  = (pupdr >> 4) & 3u;
    uint32_t pa3_pu  = (pupdr >> 6) & 3u;

    if (out_pa3_toggles)
        *out_pa3_toggles =
            (pa2_lvl << 0) | (pa3_lvl << 1) |
            (pa2_mod << 4) | (pa3_mod << 6) |
            (pa2_af  << 8) | (pa3_af  << 12) |
            (pa2_pu  << 16) | (pa3_pu << 18);
    return isr;
}

void gps_isr_stats(uint32_t *calls, uint32_t *rxne, uint32_t *fe) {
    if (calls) *calls = isr_calls;
    if (rxne)  *rxne  = rxne_count;
    if (fe)    *fe    = fe_count;
}

uint32_t gps_get_cr1(void) { return USART2_CR1; }
uint32_t gps_get_brr(void) { return USART2_BRR; }
uint32_t gps_get_nvic(void) { return NVIC_ISER; }
