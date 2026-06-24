/*
 * version.c — firmware build identity (filled in by the Makefile).
 *
 * FW_GIT_VERSION and FW_BUILD_ID are passed as -D macros at compile time.
 * This translation unit is force-rebuilt on every `make` so the build stamp
 * always reflects the latest build.
 */

#include "version.h"

#ifndef FW_GIT_VERSION
#define FW_GIT_VERSION "nogit"
#endif

#ifndef FW_BUILD_ID
#define FW_BUILD_ID "unknown"
#endif

const char *fw_version_string(void) { return FW_GIT_VERSION; }

/* Binary magic tag: server.py scans the .bin for \x00SATVER: to extract
 * the build_id. fw_build_id_string() returns a pointer into this same tag
 * so there is one copy in .rodata and --gc-sections cannot remove it. */
static const char _ota_ver_tag[] = "\x00SATVER:" FW_BUILD_ID;
const char *fw_build_id_string(void) { return _ota_ver_tag + 8; }
