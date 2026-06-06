/*
 * binfmt.c - Executable binary-format classifier (MZ / NE / PE)
 *
 * See binfmt.h. Manual header reading is the Win32s 1.25a baseline
 * (Q16: GetBinaryTypeA does not exist there); GetBinaryTypeA is an
 * uplift used only via g_features.pGetBinaryTypeA when present.
 *
 * e_lfanew is assembled from individual little-endian bytes rather than
 * cast through a struct pointer: the 512-byte read buffer is not aligned
 * for a 32-bit access at offset 0x3C, and the integer-only assembly is
 * portable to every target.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include "binfmt.h"
#include "feat.h"

/*
 * SCS_* binary-type codes returned by GetBinaryTypeA. Defined here
 * because old SDK headers (and some MinGW configs) may omit them; we
 * never link GetBinaryTypeA by name, so the constants alone are needed.
 */
#ifndef SCS_32BIT_BINARY
#define SCS_32BIT_BINARY 0
#endif
#ifndef SCS_DOS_BINARY
#define SCS_DOS_BINARY 1
#endif
#ifndef SCS_WOW_BINARY
#define SCS_WOW_BINARY 2
#endif
#ifndef SCS_PIF_BINARY
#define SCS_PIF_BINARY 3
#endif
#ifndef SCS_POSIX_BINARY
#define SCS_POSIX_BINARY 4
#endif
#ifndef SCS_OS216_BINARY
#define SCS_OS216_BINARY 5
#endif
#ifndef SCS_64BIT_BINARY
#define SCS_64BIT_BINARY 6
#endif

#define BINFMT_READ_BYTES 512

/*
 * has_path_sep - True if the name contains a directory separator.
 */
static int has_path_sep(const char *name)
{
    int i;
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '\\' || name[i] == '/' || name[i] == ':') {
            return 1;
        }
    }
    return 0;
}

/*
 * has_extension - True if the final path component contains a '.'.
 */
static int has_extension(const char *name)
{
    int i;
    int dot;
    dot = 0;
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '\\' || name[i] == '/' || name[i] == ':') {
            dot = 0;            /* reset on each component */
        } else if (name[i] == '.') {
            dot = 1;
        }
    }
    return dot;
}

/*
 * copy_str - Bounded string copy (errMsg helper).
 */
static void copy_str(char *dst, int dstSize, const char *src)
{
    int i;
    if (dst == 0 || dstSize <= 0) {
        return;
    }
    for (i = 0; i < dstSize - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * map_scs - Map a GetBinaryTypeA SCS_* code to a BinaryType.
 */
static BinaryType map_scs(DWORD scs)
{
    switch (scs) {
    case SCS_32BIT_BINARY:
        return BIN_PE32;
    case SCS_64BIT_BINARY:
        return BIN_PE32;        /* 64-bit PE; closest reportable type here */
    case SCS_DOS_BINARY:
        return BIN_MZ;
    case SCS_WOW_BINARY:
        return BIN_NE16;        /* 16-bit Windows (NE) under WoW */
    case SCS_OS216_BINARY:
        return BIN_NE16;        /* 16-bit OS/2 NE */
    default:
        return BIN_UNKNOWN;
    }
}

/*
 * classify_from_header - Read up to BINFMT_READ_BYTES from resolvedPath
 * and classify by inspecting the MZ / NE / PE headers. Returns 1 on a
 * successful read (*outType set), 0 if the file cannot be opened/read.
 */
static int classify_from_header(const char *resolvedPath, BinaryType *outType,
                                char *errMsg, int errSize)
{
    HANDLE hFile;
    unsigned char buf[BINFMT_READ_BYTES];
    DWORD got;
    long lfanew;

    hFile = CreateFileA(resolvedPath, GENERIC_READ,
                        FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        copy_str(errMsg, errSize, "cannot open file");
        return 0;
    }

    got = 0;
    if (!ReadFile(hFile, buf, (DWORD)BINFMT_READ_BYTES, &got, NULL)) {
        CloseHandle(hFile);
        copy_str(errMsg, errSize, "cannot read file");
        return 0;
    }
    CloseHandle(hFile);

    /* Need at least the 2-byte MZ magic. */
    if (got < 2 || buf[0] != 'M' || buf[1] != 'Z') {
        *outType = BIN_UNKNOWN;
        return 1;
    }

    /* e_lfanew lives at offset 0x3C, 4 bytes little-endian. Need the
       whole DOS header (0x40 bytes) to trust it. */
    if (got < 0x40) {
        *outType = BIN_MZ;
        return 1;
    }

    lfanew = (long)buf[0x3C]
           | ((long)buf[0x3D] << 8)
           | ((long)buf[0x3E] << 16)
           | ((long)buf[0x3F] << 24);

    /* Valid e_lfanew: positive, past the DOS header, and the 2-byte
       extended signature lies within the bytes we actually read. */
    if (lfanew < 0x40 || lfanew + 2 > (long)got) {
        *outType = BIN_MZ;
        return 1;
    }

    if (buf[lfanew] == 'P' && buf[lfanew + 1] == 'E'
        && lfanew + 4 <= (long)got
        && buf[lfanew + 2] == 0 && buf[lfanew + 3] == 0) {
        *outType = BIN_PE32;
    } else if (buf[lfanew] == 'N' && buf[lfanew + 1] == 'E') {
        *outType = BIN_NE16;
    } else {
        *outType = BIN_MZ;
    }
    return 1;
}

int BinFmtClassify(const char *exePath, BinaryType *outType,
                   char *errMsg, int errSize)
{
    char resolved[MAX_PATH];
    const char *toClassify;
    DWORD found;

    if (errMsg != 0 && errSize > 0) {
        errMsg[0] = '\0';
    }
    if (exePath == 0 || outType == 0) {
        copy_str(errMsg, errSize, "invalid argument");
        return 0;
    }

    /* Resolve. A name that already has a separator/extension is used as
       given; a bare name is looked up on the PATH. */
    if (has_path_sep(exePath) || has_extension(exePath)) {
        toClassify = exePath;
    } else {
        /* Bare name: try ".exe" then ".com" via SearchPathA. If neither
           resolves, it is a shell built-in - no file read (e.g. "dir"). */
        found = SearchPathA(NULL, exePath, ".exe",
                            (DWORD)MAX_PATH, resolved, NULL);
        if (found == 0 || found >= (DWORD)MAX_PATH) {
            found = SearchPathA(NULL, exePath, ".com",
                                (DWORD)MAX_PATH, resolved, NULL);
        }
        if (found == 0 || found >= (DWORD)MAX_PATH) {
            *outType = BIN_SHELL;
            return 1;
        }
        toClassify = resolved;
    }

    /* Uplift: prefer the kernel's classifier when present. It resolves
       the path itself and knows about WoW64. Fall back to manual header
       reading when the pointer is NULL or the call fails. */
    if (g_features.pGetBinaryTypeA != 0) {
        DWORD scs;
        scs = 0;
        if (g_features.pGetBinaryTypeA(toClassify, &scs)) {
            *outType = map_scs(scs);
            return 1;
        }
    }

    return classify_from_header(toClassify, outType, errMsg, errSize);
}
