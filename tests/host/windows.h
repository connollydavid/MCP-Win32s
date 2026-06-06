/*
 * windows.h - host-build shim shim.
 *
 * Resolves `#include <windows.h>` in the C89 modules under test (src/catalog.c)
 * when they are compiled NATIVELY on Linux for the theft host PBT harness.
 * See win32_shim.h for the actual minimal Win32 surface. Linux has no real
 * windows.h, so -Itests/host makes this file satisfy the include.
 *
 * This is free and unencumbered software released into the public domain.
 */
#ifndef WIN32_SHIM_WINDOWS_H
#define WIN32_SHIM_WINDOWS_H
#include "win32_shim.h"
#endif
