/*
 * crc32.c — IEEE 802.3 CRC-32, bytewise (no table, to keep .data small).
 *
 * Bit-for-bit identical to Python's zlib.crc32, so the OTA server can compute
 * the expected image CRC and the target can verify the bytes it stored.
 */

#include "crc32.h"

uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
