/*
 * ota_apply.h — boot-time application of a staged OTA image.
 *
 * Called very early in Reset_Handler (after .data/.ramfunc are in RAM, before
 * the watchdog is started). If an OTA image is pending in the staging bank and
 * the running application does not already match it, a RAM-resident routine
 * erases the application flash, copies the staged image into place, and resets.
 *
 * Power-fail safety: the staging bank is the golden copy. The pending flag is
 * cleared only once the application flash is confirmed identical to the staged
 * image, so an interruption mid-copy simply retries on the next boot.
 */

#ifndef OTA_APPLY_H
#define OTA_APPLY_H

void ota_apply_at_boot(void);

/* Validate the flash erase+program-from-flash sequence on a scratch bank page
 * without touching the running application. Returns true on success. */
#include <stdbool.h>
bool ota_self_test(void);

#endif /* OTA_APPLY_H */
