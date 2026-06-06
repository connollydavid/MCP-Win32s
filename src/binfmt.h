/*
 * binfmt.h - Executable binary-format classifier (MZ / NE / PE)
 *
 * Classifies an executable by reading its DOS/NE/PE headers, so the
 * exec path can report binary_type and decide how to treat a child
 * (e.g. 16-bit DOS/NE in a shared VDM must not be TerminateProcess'd
 * on timeout - Q12). On NT/95+ the kernel's GetBinaryTypeA is preferred
 * when present (it knows about WoW64); on Win32s 1.25a GetBinaryTypeA is
 * absent, so we fall back to a manual header read (Q16). GetBinaryTypeA
 * is reached only through g_features.pGetBinaryTypeA - never by name at
 * link time, or the binary would not load on Win32s.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef BINFMT_H
#define BINFMT_H

typedef enum {
    BIN_UNKNOWN = 0,
    BIN_PE32    = 1,
    BIN_NE16    = 2,
    BIN_MZ      = 3,
    BIN_SHELL   = 4
} BinaryType;

/*
 * BinFmtClassify - Classify the executable named by exePath.
 *
 * Resolution:
 *   - A bare name (no path separator, no extension) that SearchPathA
 *     cannot resolve as ".exe"/".com" is treated as a shell built-in:
 *     *outType = BIN_SHELL, no file is read, returns 1.
 *   - Otherwise the path is resolved (SearchPathA for a bare name,
 *     used verbatim if it already contains a separator/extension) and
 *     the first bytes are read via CreateFileA/ReadFile:
 *       MZ + valid e_lfanew -> magic at e_lfanew: "PE\0\0"=BIN_PE32,
 *         "NE"=BIN_NE16, else BIN_MZ.
 *       MZ + invalid e_lfanew -> BIN_MZ.
 *       no MZ -> BIN_UNKNOWN (still returns 1 for a readable file).
 *
 * On NT/95+ g_features.pGetBinaryTypeA is preferred when non-NULL and
 * the call succeeds; manual header reading is the fallback.
 *
 * Returns 1 on success (*outType set). Returns 0 on failure (resolved
 * file missing or unreadable); errMsg receives a short description.
 */
int BinFmtClassify(const char *exePath, BinaryType *outType,
                   char *errMsg, int errSize);

#endif /* BINFMT_H */
