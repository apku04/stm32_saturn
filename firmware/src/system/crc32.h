/*
 * crc32.h — IEEE 802.3 CRC-32 (matches Python zlib.crc32 / the OTA server).
 *
 * Reflected, polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 * Used to verify a complete OTA image in flash against the CRC the sender
 * computed over the same bytes.
 */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/* One-shot CRC-32 over a buffer. */
uint32_t crc32_compute(const uint8_t *data, uint32_t len);

#endif /* CRC32_H */
