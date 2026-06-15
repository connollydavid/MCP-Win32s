/*
 * charset_tables.h - Embedded code-page <-> Unicode tables
 *
 * GENERATED DATA. The tables in src/charset_tables.c are produced by
 * tools/gen_charset_tables.py from the vendored Unicode Consortium MICSFT
 * mappings (vendor/charset-mappings/). Do NOT hand-edit charset_tables.c -
 * edit the generator and regenerate. This header is the stable, hand-written
 * contract over that data.
 *
 * The device owns the entire text pipeline and bakes its own tables rather than
 * calling the OS's MultiByteToWideChar/WideCharToMultiByte, so the mapping is
 * deterministic and identical on every Windows target (Win3.1+Win32s 1.25a ->
 * Win11). Spec: encoding.allium contract CodepageTables, pinning
 * CodepageRoundTripsOnBijectiveSubset.
 *
 * Hard constraints (this binary's own source, per CLAUDE.md): C89, declarations
 * at block top, slash-star comments only, no floating point, i386, no threads.
 * PURE: no Win32, no FP - so it compiles in the native theft host harness too.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#ifndef CHARSET_TABLES_H
#define CHARSET_TABLES_H

/*
 * CP_REPLACEMENT - U+FFFD REPLACEMENT CHARACTER. Emitted for an undefined byte
 * position or a malformed DBCS sequence on decode (CodepageRoundTripsOnBijective-
 * Subset: codepage_to_unicode is TOTAL - it never fails, only substitutes).
 */
#define CP_REPLACEMENT 0xFFFDu

/*
 * CpSupported - 1 iff `codepage` is one of the baked pages, else 0. The baked
 * set is the full historical Windows single-byte inventory plus the four CJK
 * DBCS pages:
 *   ANSI single-byte : 1250 1251 1252 1253 1254 1255 1256 1257 1258 874
 *   OEM/DOS single-byte: 437 737 775 850 852 855 857 858 860 861 862 863 864
 *                        865 866 869
 *   CJK DBCS          : 932 936 949 950
 */
int CpSupported(int codepage);

/*
 * CpDecode - codepage bytes -> UTF-16 code units (spec: codepage_to_unicode).
 *
 * TOTAL: an undefined single byte, an undefined DBCS position, a lead byte with
 * no following trail, or a trail byte out of its lead's range each yield exactly
 * one CP_REPLACEMENT and the scan advances by one byte (the maximal-subpart
 * discipline, mirroring the UTF-8 codec). A valid SBCS byte -> one unit; a valid
 * DBCS lead+trail -> one unit (every baked DBCS page is BMP, so a decode never
 * produces a surrogate pair).
 *
 * Writes at most outCap units. Returns the number of code units written.
 * *lossy (if non-NULL) is set to 1 when any CP_REPLACEMENT was substituted,
 * else 0. *consumed (if non-NULL) is set to the number of input bytes consumed
 * (== nBytes unless outCap stopped the scan early, always on a whole-character
 * boundary). codepage MUST satisfy CpSupported.
 */
int CpDecode(int codepage, const unsigned char *mb, int nBytes,
             unsigned short *out, int outCap, int *lossy, int *consumed);

/*
 * CpEncode - UTF-16 code units -> codepage bytes (spec: unicode_to_codepage).
 *
 * STRICT: a code unit the target page cannot represent stops the encode - the
 * function returns -1 with *rejectAt (if non-NULL) set to that unit's index. No
 * '?' substitution, no output past the boundary (StrictNarrowingRejectsUnrepre-
 * sentable: the device never silently mis-targets a file). A lone surrogate
 * (0xD800-0xDFFF) is representable in no baked page, so it rejects. When a code
 * unit has more than one byte representation (vendor-divergent / NEC-IBM
 * duplicate rows), the Microsoft-canonical sequence is emitted
 * (docs/charset-bijection.md), so an inbound path round-trips to the bytes the
 * OS itself would use.
 *
 * On full success returns the number of bytes written and sets *rejectAt = -1;
 * the output is whole. Returns -2 if outCap would overflow (buffer too small;
 * never a partial multi-byte sequence). codepage MUST satisfy CpSupported.
 */
int CpEncode(int codepage, const unsigned short *units, int nUnits,
             unsigned char *out, int outCap, int *rejectAt);

/*
 * CpRepresentable - the `representable` predicate (spec:
 * StrictNarrowingRejectsUnrepresentable): 1 iff every code unit in units has a
 * byte representation in codepage (so CpEncode would not reject), else 0. Pure
 * over the reverse map. codepage MUST satisfy CpSupported.
 */
int CpRepresentable(int codepage, const unsigned short *units, int nUnits);

#endif /* CHARSET_TABLES_H */
