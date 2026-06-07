/*
 * encoding.h - Full-range text encoding for MCP-Win32s (work-item 5.4)
 *
 * The device owns the ENTIRE text pipeline and emits valid UTF-8 on every wire
 * surface across the whole target range (Win3.1+Win32s 1.25a -> Win11), using
 * our OWN embedded C89 tables (charset_tables.h) - never the OS's
 * MultiByteToWideChar/WideCharToMultiByte. Spec: encoding.allium.
 *
 * Two halves:
 *   - PURE (always compiled, incl. the native theft host harness when
 *     ENCODING_HOST_PURE is defined): the UTF-8<->UTF-16 codec and the
 *     byte-wise path-separator scan. No Win32, no FP, no CP_UTF8 dependency.
 *   - TIER (Win32 only; excluded under ENCODING_HOST_PURE): the OS-family
 *     conversion tier + the compound call-site helpers (EncOpenPath /
 *     EncBytesToWire / EncWideToWire) the file/exec ops use, reading g_features
 *     and the delay-loaded -W uplift (feat.h).
 *
 * Hard constraints (this binary's own source, per CLAUDE.md): C89, declarations
 * at block top, slash-star comments only, no floating point, i386, no threads.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#ifndef ENCODING_H
#define ENCODING_H

#include "charset_tables.h"   /* CP_REPLACEMENT + the cp<->Unicode tables */

/* ------------------------------------------------------------------ */
/* Enumerations (spec: encoding.allium)                                */
/* ------------------------------------------------------------------ */

/*
 * EncTier - the device's conversion tier, from the OS family (spec: enum
 * EncodingTier). manifest: Win10 1903+ with activeCodePage=UTF-8 (the -A APIs
 * speak UTF-8, no table). wide: NT..8.1, the delay-loaded -W APIs (full Unicode
 * I/O). codepage: Win32s/Win9x, our own tables widen/narrow the ANSI/OEM page.
 * On EVERY tier the device emits valid UTF-8.
 */
typedef enum {
    ENC_TIER_MANIFEST = 0,
    ENC_TIER_WIDE,
    ENC_TIER_CODEPAGE
} EncTier;

/*
 * EncStatus - the outcome of one conversion (spec: enum ConversionStatus).
 */
typedef enum {
    ENC_OK = 0,        /* clean, fully representable */
    ENC_LOSSY,         /* a decode substituted U+FFFD (a result was produced) */
    ENC_REJECTED,      /* an inbound path could not narrow (unrepresentable) */
    ENC_TRUNCATED      /* output hit a buffer bound; cut on a code-point boundary */
} EncStatus;

/*
 * EncDirection - which way text crosses the device boundary (spec: enum
 * ConversionDirection).
 */
typedef enum {
    ENC_INBOUND = 0,   /* agent UTF-8 -> a file/process API */
    ENC_OUTBOUND       /* OS name/listing/exec-output -> the UTF-8 wire */
} EncDirection;

/* ------------------------------------------------------------------ */
/* The pure codec (spec: contract Utf8Codec) - always available.       */
/* ------------------------------------------------------------------ */

/*
 * Utf16ToUtf8 - Encode UTF-16 code units to UTF-8 (spec: utf16_to_utf8).
 *   NeverEmitInvalidUtf8: a lone/unpaired surrogate is emitted as U+FFFD, never
 *     a CESU-8/WTF-8 sequence - every byte written is well-formed UTF-8.
 *   TruncationIsBoundaryClean: if outCap would be exceeded, the encode stops on
 *     a code-point boundary (never mid-sequence) and *status = ENC_TRUNCATED.
 * Returns the number of bytes written; *status (if non-NULL) = ENC_OK, ENC_LOSSY
 * (a surrogate was replaced) or ENC_TRUNCATED. Pure.
 */
int Utf16ToUtf8(const unsigned short *units, int nUnits,
                unsigned char *out, int outCap, EncStatus *status);

/*
 * Utf8ToUtf16 - Decode UTF-8 bytes to UTF-16 code units (spec: utf8_to_utf16).
 *   LossyDecodeIsTotal: never fails or loops on ARBITRARY bytes - a malformed
 *     maximal subpart yields exactly one U+FFFD and the scanner advances to the
 *     next lead byte, so it always makes progress.
 *   TruncationIsBoundaryClean: stops on a code-unit boundary (never a split
 *     surrogate pair) when outCap is reached; *status = ENC_TRUNCATED.
 * A code point above U+FFFF is emitted as a surrogate pair (two units). Returns
 * the number of code units written; *status (if non-NULL) = ENC_OK, ENC_LOSSY
 * or ENC_TRUNCATED. Pure.
 */
int Utf8ToUtf16(const unsigned char *bytes, int nBytes,
                unsigned short *out, int outCap, EncStatus *status);

/* ------------------------------------------------------------------ */
/* The path-separator scan (spec: contract PathScanning) - pure.       */
/* ------------------------------------------------------------------ */

/*
 * EncFindSeparators - Scan a UTF-8 path and write the byte offsets of true path
 * separators ('\\' and '/') to out (spec: find_separators /
 * PathSeparatorScanIsDbcsSafe). DBCS-safe by construction: the scan runs on the
 * self-synchronising UTF-8 form, where a continuation byte (0x80-0xBF) can never
 * be mistaken for 0x5C, so a DBCS trail byte's 0x5C is never counted as a
 * separator. Returns the number of separators found (writing at most outCap
 * offsets to out); pure.
 */
int EncFindSeparators(const unsigned char *utf8Path, int len,
                      int *out, int outCap);

#ifndef ENCODING_HOST_PURE
/* ================================================================== */
/* TIER surface (Win32 only) - the OS-family tier + the call-site      */
/* compound helpers. Excluded from the pure host harness.              */
/* ================================================================== */
#include <windows.h>

/*
 * Path buffer sizes. The device's path limit is MCP_MAX_PATH_LEN == MAX_PATH ==
 * 260. A UTF-8 path is at most 4 bytes per code point.
 */
#define ENC_PATH_WIDE  260                 /* WCHAR units (incl. NUL) */
#define ENC_PATH_MB    (260 * 4 + 1)       /* UTF-8 / ANSI bytes + NUL */

/*
 * EncTierCurrent - the device's tier from the OS family + manifest state:
 *   manifest  - is_nt && GetACP()==65001 (the activeCodePage manifest is live)
 *   wide      - is_nt otherwise AND the -W file uplift is present
 *               (g_features.has_wide_fileapi)
 *   codepage  - is_win32s / is_win9x, OR an NT host with the -W uplift forced
 *               off (FEAT_FORCE_NO_WIDE_FILEAPI) - our own tables narrow/widen.
 * Read from g_features (feat.h); FeatInit must have run.
 */
EncTier EncTierCurrent(void);

/* EncTierName - the tier's name ("manifest"|"wide"|"codepage"). Static. */
const char *EncTierName(EncTier t);

/*
 * EncProvenanceTag - the wire-contract ReadyShape `encoding` provenance tag for
 * the ready message: "utf8_manifest" (manifest), "utf8_via_w" (wide),
 * "utf8_from_cp" (codepage). INFORMATIONAL only - the device emits UTF-8 on
 * every tier regardless; the bridge never transcodes on it (wire-contract.allium).
 */
const char *EncProvenanceTag(void);

/* EncActiveCodePage - GetACP(). EncConsoleOutputCp - the child console output
 * code page for exec-output transcode (GetConsoleOutputCP, else GetOEMCP). */
unsigned int EncActiveCodePage(void);
unsigned int EncConsoleOutputCp(void);

/* EncManifestUtf8Active - 1 iff GetACP()==65001 (the Win10 manifest is live). */
int EncManifestUtf8Active(void);

/*
 * EncPathForm - the form an inbound agent path takes for a file/process API on
 * the current tier (the output of EncOpenPath).
 */
typedef struct {
    int       useWide;             /* 1 -> use wOut with a -W API (wide tier) */
    EncStatus status;              /* ENC_OK | ENC_REJECTED | ENC_LOSSY */
    int       rejectAt;            /* offending UTF-16 unit index on ENC_REJECTED */
    WCHAR     wOut[ENC_PATH_WIDE]; /* NUL-terminated UTF-16 (useWide==1) */
    char      mbOut[ENC_PATH_MB];  /* NUL-terminated bytes for an -A API
                                    * (useWide==0): UTF-8 on the manifest tier,
                                    * the narrowed ANSI page on the codepage tier */
} EncPathForm;

/*
 * EncOpenPath - Prepare an inbound agent UTF-8 path for a file/process API on
 * the device's tier (spec: rule PathConverted, inbound). On the:
 *   manifest tier - mbOut = the UTF-8 bytes unchanged (the -A APIs speak UTF-8),
 *                   useWide = 0.
 *   wide tier     - wOut  = the widened UTF-16, useWide = 1 (CreateFileW et al.).
 *   codepage tier - mbOut = the UTF-8 widened to UTF-16 then narrowed to
 *                   GetACP(); useWide = 0. A code point the page cannot represent
 *                   -> status = ENC_REJECTED, rejectAt set, NO usable output
 *                   (StrictNarrowingRejectsUnrepresentable - the caller must
 *                   touch no file).
 * `out` must be non-NULL. No file is opened here; this only prepares the form.
 */
void EncOpenPath(const char *utf8Path, EncPathForm *out);

/*
 * EncBytesToWire - Render OS-sourced bytes in code page srcCp to the UTF-8 wire
 * (spec: rule OutputConverted, outbound). Used for an -A directory listing /
 * file name (srcCp = GetACP() on the codepage tier) and for child exec output
 * (srcCp = the child console CP). srcCp == 65001 (CP_UTF8, the manifest tier's
 * already-UTF-8 -A bytes) is validated through the codec rather than table-
 * decoded. The result is always well-formed UTF-8 (NeverEmitInvalidUtf8); an
 * undefined source byte -> U+FFFD with *status = ENC_LOSSY (never a failure).
 * Returns the number of bytes written to out (at most outCap; *status =
 * ENC_TRUNCATED, cut on a code-point boundary, if it would overflow).
 */
int EncBytesToWire(unsigned int srcCp, const unsigned char *mb, int len,
                   char *out, int outCap, EncStatus *status);

/*
 * EncWideToWire - Render an OS-sourced UTF-16 result (a FindFirstFileW name on
 * the wide tier) to the UTF-8 wire (spec: rule OutputConverted, outbound). Thin
 * wrapper over Utf16ToUtf8 - always well-formed UTF-8. Returns bytes written;
 * *status as Utf16ToUtf8.
 */
int EncWideToWire(const unsigned short *units, int n,
                  char *out, int outCap, EncStatus *status);

#endif /* !ENCODING_HOST_PURE */

#endif /* ENCODING_H */
