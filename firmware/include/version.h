/*
 * version.h — firmware build identity
 *
 * The strings are filled in at build time from the Makefile (git describe +
 * UTC build timestamp). Use the `version` CDC command to read them back from
 * a running target so a flash can be verified.
 */

#ifndef VERSION_H
#define VERSION_H

/* Source identity, e.g. "a1b2c3d4" or "v1.2-3-ga1b2c3d4-dirty". */
const char *fw_version_string(void);

/* UTC build stamp, e.g. "20260605T103000Z" — changes on every build. */
const char *fw_build_id_string(void);

#endif /* VERSION_H */
