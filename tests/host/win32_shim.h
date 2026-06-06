/*
 * win32_shim.h - Minimal Win32 shim for the host-native theft PBT harness.
 *
 * Phase 4 theft host harness (plan/PHASE4.md, "theft host-side PBT harness").
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
static HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share,
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
static BOOL ReadFile(HANDLE h, void *buf, DWORD toRead, LPDWORD read,
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

static BOOL CloseHandle(HANDLE h)
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
static char *lstrcpynA(char *dst, const char *src, int count)
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

#endif /* WIN32_SHIM_H */
