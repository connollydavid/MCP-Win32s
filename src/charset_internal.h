/*
 * charset_internal.h - data layout shared by the generated tables
 * (charset_tables_data.c) and the hand-written logic (charset_tables.c).
 *
 * NOT a public interface (the public contract is charset_tables.h). This only
 * declares the in-memory shape of the generated data so the logic can walk it.
 *
 * Hard constraints: C89, no FP, pure (no Win32) - compiles in the host harness.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#ifndef CHARSET_INTERNAL_H
#define CHARSET_INTERNAL_H

/*
 * CpRevEntry - one reverse-map entry: a UTF-16 code unit -> its canonical
 * code-page byte sequence. mb holds a single byte (<= 0xFF) or a double byte
 * (lead<<8 | trail, always >= 0x8100 since every baked DBCS lead is >= 0x81),
 * so the byte count is inferable: mb > 0xFF iff two bytes. Entries are sorted
 * ascending by cu within a page for binary search.
 */
typedef struct {
    unsigned short cu;       /* the UTF-16 code unit (BMP) */
    unsigned short mb;       /* single byte, or (lead<<8)|trail */
} CpRevEntry;

/*
 * CpPage - one baked code page.
 *   SBCS (is_dbcs==0): dec1[256] maps each byte to its unit (CP_REPLACEMENT for
 *     an undefined position); lead/leadrow/dec2 unused.
 *   DBCS (is_dbcs==1): lead[b]==1 marks a lead byte; for a lead, leadrow[b] is
 *     the row index into dec2 (a flat ndec2*256 array of trail units); dec1[b]
 *     is the single-byte unit for a non-lead byte.
 * rev is the sorted reverse map (nrev entries).
 */
typedef struct {
    int                  cp;
    int                  is_dbcs;
    const unsigned short *dec1;     /* [256] */
    const unsigned char  *lead;     /* [256] (DBCS; 0 for SBCS) */
    const short          *leadrow;  /* [256] row index or -1 (DBCS; 0 for SBCS) */
    const unsigned short *dec2;     /* [ndec2*256] trail tables (DBCS) */
    int                  ndec2;
    const CpRevEntry     *rev;      /* [nrev], sorted by cu */
    int                  nrev;
} CpPage;

extern const CpPage charset_pages[];
extern const int charset_page_count;

#endif /* CHARSET_INTERNAL_H */
