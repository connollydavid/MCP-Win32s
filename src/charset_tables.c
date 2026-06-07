/*
 * charset_tables.c - hand-written decode/encode logic over the generated
 * code-page tables (charset_tables_data.c). Spec: encoding.allium contract
 * CodepageTables (CodepageRoundTripsOnBijectiveSubset).
 *
 * The tables are cited DATA (generated from the Unicode Consortium MICSFT
 * mappings); the algorithms here are the reviewed logic. Both halves are pure
 * C89 (no Win32, no FP), so this compiles in the native theft host harness and
 * on the i386/Win32s target identically.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#include "charset_tables.h"
#include "charset_internal.h"

static const CpPage *find_page(int codepage)
{
    int i;
    for (i = 0; i < charset_page_count; i++) {
        if (charset_pages[i].cp == codepage) {
            return &charset_pages[i];
        }
    }
    return 0;
}

int CpSupported(int codepage)
{
    return find_page(codepage) != 0;
}

int CpDecode(int codepage, const unsigned char *mb, int nBytes,
             unsigned short *out, int outCap, int *lossy, int *consumed)
{
    const CpPage *p;
    int i;
    int n;
    int lz;

    i = 0;
    n = 0;
    lz = 0;
    if (lossy != 0) {
        *lossy = 0;
    }
    if (consumed != 0) {
        *consumed = 0;
    }
    p = find_page(codepage);
    if (p == 0) {
        return 0;               /* caller guarantees CpSupported; defensive */
    }

    while (i < nBytes && n < outCap) {
        unsigned char b;
        unsigned short u;

        b = mb[i];
        if (p->is_dbcs && p->lead[b]) {
            if (i + 1 >= nBytes) {
                /* a lead byte with no trail: U+FFFD, resync one byte. */
                u = CP_REPLACEMENT;
                lz = 1;
                i += 1;
            } else {
                short row;
                unsigned char t;

                row = p->leadrow[b];
                t = mb[i + 1];
                u = p->dec2[(int)row * 256 + (int)t];
                if (u == CP_REPLACEMENT) {
                    /* lead + undefined trail: U+FFFD, resync one byte (the
                     * trail may itself begin a valid character). */
                    lz = 1;
                    i += 1;
                } else {
                    i += 2;
                }
            }
        } else {
            u = p->dec1[b];
            if (u == CP_REPLACEMENT) {
                lz = 1;
            }
            i += 1;
        }
        out[n] = u;
        n += 1;
    }

    if (lossy != 0) {
        *lossy = lz;
    }
    if (consumed != 0) {
        *consumed = i;
    }
    return n;
}

/* Binary search the sorted reverse map for a code unit. Returns the mb value
 * (1- or 2-byte, inferable from mb > 0xFF) or -1 when not representable. */
static long rev_lookup(const CpPage *p, unsigned short cu)
{
    int lo;
    int hi;

    lo = 0;
    hi = p->nrev - 1;
    while (lo <= hi) {
        int midi;
        unsigned short midcu;

        midi = lo + (hi - lo) / 2;
        midcu = p->rev[midi].cu;
        if (midcu == cu) {
            return (long)p->rev[midi].mb;
        } else if (midcu < cu) {
            lo = midi + 1;
        } else {
            hi = midi - 1;
        }
    }
    return -1L;
}

int CpEncode(int codepage, const unsigned short *units, int nUnits,
             unsigned char *out, int outCap, int *rejectAt)
{
    const CpPage *p;
    int i;
    int n;

    if (rejectAt != 0) {
        *rejectAt = -1;
    }
    p = find_page(codepage);
    if (p == 0) {
        if (rejectAt != 0) {
            *rejectAt = 0;
        }
        return -1;
    }

    n = 0;
    for (i = 0; i < nUnits; i++) {
        long mb;

        mb = rev_lookup(p, units[i]);
        if (mb < 0) {
            if (rejectAt != 0) {
                *rejectAt = i;
            }
            return -1;          /* not representable: strict reject */
        }
        if (mb > 0xFF) {
            if (n + 2 > outCap) {
                return -2;      /* never a partial multi-byte sequence */
            }
            out[n] = (unsigned char)((mb >> 8) & 0xFF);
            out[n + 1] = (unsigned char)(mb & 0xFF);
            n += 2;
        } else {
            if (n + 1 > outCap) {
                return -2;
            }
            out[n] = (unsigned char)(mb & 0xFF);
            n += 1;
        }
    }
    return n;
}

int CpRepresentable(int codepage, const unsigned short *units, int nUnits)
{
    const CpPage *p;
    int i;

    p = find_page(codepage);
    if (p == 0) {
        return 0;
    }
    for (i = 0; i < nUnits; i++) {
        if (rev_lookup(p, units[i]) < 0) {
            return 0;
        }
    }
    return 1;
}
