/*
 * win32_shim.h - Minimal Win32 shim for the host-native theft PBT harness.
 *
 * theft host harness (plan/PHASE4.md, "theft host-side PBT harness").
 *
 * The shipped modules under test stay strict C89 and #include <windows.h>
 * (src/catalog.c) or use the Win32 ANSI string helpers. theft itself is
 * C99/POSIX and runs NATIVELY on Linux with gcc -- there is no <windows.h>
 * there. This header provides the *minimal* subset of the Win32 surface that
 * src/catalog.c touches, mapped onto stdio / ANSI C, so the C89 sources
 * compile UNMODIFIED into the host build.
 *
 * The source line `#include <windows.h>` in catalog.c resolves to the
 * sibling tests/host/windows.h shim (a one-line include of this file),
 * picked up because -Itests/host is on the include path and Linux has no
 * real windows.h. Nothing in src/ is patched.
 *
 * Surface actually used by src/catalog.c:
 *   types/macros : HANDLE, DWORD, BOOL, LPVOID, NULL-ish constants,
 *                  GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
 *                  FILE_ATTRIBUTE_NORMAL, INVALID_HANDLE_VALUE, TRUE/FALSE
 *   file I/O     : CreateFileA, ReadFile, CloseHandle  (over <stdio.h>)
 *   ANSI strings : lstrcpynA, lstrcmpA, lstrcmpiA, lstrlenA
 *
 * This is free and unencumbered software released into the public domain.
 */

#ifndef WIN32_SHIM_H
#define WIN32_SHIM_H

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

/* Each host test TU touches only the subset of this shim it needs (catalog
 * uses the file I/O + ANSI strings; strutil uses only CharNextA), so under
 * -Werror an unused static here is an error. Mark the static helpers
 * possibly-unused; this is a gcc-only host harness (CLAUDE.md "two frameworks"). */
#if defined(__GNUC__)
#define SHIM_UNUSED __attribute__((__unused__))
#else
#define SHIM_UNUSED
#endif

/* ---- Win32 scalar types (host approximations) ---- */
typedef void *        HANDLE;
typedef unsigned long DWORD;
typedef DWORD *       LPDWORD;
typedef int           BOOL;
typedef void *        LPVOID;
typedef const char *  LPCSTR;
typedef char *        LPSTR;
typedef void *        LPSECURITY_ATTRIBUTES;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* CreateFileA argument flags -- values are irrelevant to the shim, which
 * only ever opens the file read-only, but the symbols must exist. */
#define GENERIC_READ          0x80000000UL
#define FILE_SHARE_READ       0x00000001UL
#define OPEN_EXISTING         3
#define FILE_ATTRIBUTE_NORMAL 0x00000080UL

/* A FILE* never collides with this sentinel, so it is a safe "no handle". */
#define INVALID_HANDLE_VALUE  ((HANDLE)(-1))

/*
 * CreateFileA - open PATH read-only. Only the read-only path catalog.c uses
 * is honoured; the access/share/disposition flags are ignored.
 */
static SHIM_UNUSED HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share,
                          LPSECURITY_ATTRIBUTES sa, DWORD disp,
                          DWORD attrs, HANDLE tmpl)
{
    FILE *f;
    (void)access; (void)share; (void)sa; (void)disp; (void)attrs; (void)tmpl;
    f = fopen(path, "rb");
    if (f == NULL) {
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)f;
}

/*
 * ReadFile - read up to toRead bytes; *read set to the count, like Win32.
 * Returns FALSE only on a hard read error (matches catalog.c's expectations:
 * EOF is read==0 with a TRUE return).
 */
static SHIM_UNUSED BOOL ReadFile(HANDLE h, void *buf, DWORD toRead, LPDWORD read,
                     LPVOID overlapped)
{
    size_t n;
    FILE *f = (FILE *)h;
    (void)overlapped;
    n = fread(buf, 1, (size_t)toRead, f);
    if (n == 0 && ferror(f)) {
        if (read != NULL) { *read = 0; }
        return FALSE;
    }
    if (read != NULL) { *read = (DWORD)n; }
    return TRUE;
}

static SHIM_UNUSED BOOL CloseHandle(HANDLE h)
{
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    fclose((FILE *)h);
    return TRUE;
}

/* ---- Win32 ANSI string helpers mapped onto ANSI C / POSIX ---- */

/* lstrcpynA copies at most count-1 chars and always NUL-terminates (Win32
 * semantics), returning the destination. */
static SHIM_UNUSED char *lstrcpynA(char *dst, const char *src, int count)
{
    int i;
    if (count <= 0) {
        return dst;
    }
    for (i = 0; i < count - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return dst;
}

#define lstrcmpA(a, b)  strcmp((a), (b))
#define lstrcmpiA(a, b) strcasecmp((a), (b))
#define lstrlenA(s)     ((int)strlen(s))

/*
 * CharNextA - DBCS-aware single-character step (Win32 LPSTR CharNextA(LPCSTR)).
 * src/strutil.c steps with this to never split a multibyte character.
 *
 * The host harness is native Linux with no real CharNextA. To make the
 * no-split property DETERMINISTICALLY testable, this shim models a cp932-style
 * DBCS codepage: a lead byte in 0x81-0x9F or 0xE0-0xFC followed by a non-NUL
 * trail byte advances by 2 bytes (one double-byte character); otherwise it
 * advances by 1. At the terminating NUL it does not advance past it. This is
 * the same lead-byte test the real CharNextA applies under a DBCS ACP.
 */
static SHIM_UNUSED char *CharNextA(const char *p)
{
    unsigned char lead;
    if (p == NULL) {
        return (char *)p;
    }
    if (*p == '\0') {
        return (char *)p;           /* never step past the terminator */
    }
    lead = (unsigned char)*p;
    if (((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC)) &&
        p[1] != '\0') {
        return (char *)(p + 2);     /* whole double-byte character */
    }
    return (char *)(p + 1);
}

#endif /* WIN32_SHIM_H */
