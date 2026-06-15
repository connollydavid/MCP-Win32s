/*
 * strutil.c - small string helpers the native-Win32 floor lacks. See strutil.h.
 *
 * Windows NT 3.1's kernel32 (the native-Win32 floor, July 1993) does not export
 * lstrcpynA - it arrived in NT 3.51/Win95 - so the device carries its own. The
 * copy is DBCS-aware (steps with CharNextA) so a truncated result never leaves a
 * dangling double-byte lead, matching the real lstrcpynA. C89 / i386 / ANSI only.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#include <windows.h>
#include "strutil.h"

char *McpStrCpyN(char *dst, const char *src, int n)
{
    int i;
    const char *p;
    const char *q;
    int clen;

    if (dst == NULL || n <= 0) {
        return dst;
    }
    i = 0;
    p = src;
    while (p != NULL && *p != '\0') {
        q = CharNextA(p);           /* DBCS-aware single-character step */
        clen = (int)(q - p);
        if (i + clen > n - 1) {
            break;                  /* next char would not fit with the NUL */
        }
        while (p < q) {
            dst[i] = *p;
            i++;
            p++;
        }
    }
    dst[i] = '\0';
    return dst;
}
