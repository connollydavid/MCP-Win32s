/*
 * test_pbt_encoding.c - property-based tests for the pure UTF-8 <-> UTF-16
 * codec (src/encoding.c: Utf16ToUtf8 / Utf8ToUtf16), on the shipped C89 path.
 *
 * Mirrors the four theft host properties (tests/host/theft_encoding.c) at lower
 * trial counts, so the codec's SAFETY pins (encoding.allium contract Utf8Codec)
 * are proven on the actual C89/i386 build, not only natively:
 *   codec_round_trip       Utf8ToUtf16(Utf16ToUtf8(u)) == u on the well-formed
 *                          UTF-16 subset, ENC_OK both ways.
 *   codec_decode_total     LossyDecodeIsTotal: arbitrary bytes decode without a
 *                          crash, consuming the whole input (a byte-cursor
 *                          oracle re-walks Table 3-7), emitting no lone surrogate.
 *   codec_never_invalid    NeverEmitInvalidUtf8: arbitrary units encode to
 *                          well-formed UTF-8 (an in-test validator finds zero
 *                          ill-formed sequences).
 *   codec_truncation_clean TruncationIsBoundaryClean: a capped encode/decode
 *                          leaves a whole valid prefix; no split surrogate pair.
 *
 * Uses prop.h (minimal C89 PBT framework). Pure - links only src/encoding.c.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#define PROP_IMPLEMENTATION
#include "prop.h"
#include "encoding.h"
#include <stdio.h>

#define MAX_CP  64
#define MAX_U   192
#define MAX_B   256

/* oracle_step - classify the sequence at bytes[i] of buffer length n, per
 * Unicode Table 3-7, independently of the codec. *adv = bytes to advance,
 * *cp = decoded code point (when well-formed). Returns 1 if well-formed, else 0
 * (an ill-formed maximal subpart of length *adv). */
static int oracle_step(const unsigned char *bytes, int n, int i,
                       int *adv, unsigned long *cp)
{
    unsigned int  b0 = bytes[i];
    int           seqLen, k;
    unsigned int  lo2, hi2;
    unsigned long acc;

    if (b0 <= 0x7Fu) {
        *adv = 1; *cp = b0; return 1;
    }
    seqLen = 0; lo2 = 0x80u; hi2 = 0xBFu; acc = 0;
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        seqLen = 2; acc = b0 & 0x1Fu;
    } else if (b0 == 0xE0u) {
        seqLen = 3; lo2 = 0xA0u; acc = b0 & 0x0Fu;
    } else if (b0 >= 0xE1u && b0 <= 0xECu) {
        seqLen = 3; acc = b0 & 0x0Fu;
    } else if (b0 == 0xEDu) {
        seqLen = 3; hi2 = 0x9Fu; acc = b0 & 0x0Fu;
    } else if (b0 >= 0xEEu && b0 <= 0xEFu) {
        seqLen = 3; acc = b0 & 0x0Fu;
    } else if (b0 == 0xF0u) {
        seqLen = 4; lo2 = 0x90u; acc = b0 & 0x07u;
    } else if (b0 >= 0xF1u && b0 <= 0xF3u) {
        seqLen = 4; acc = b0 & 0x07u;
    } else if (b0 == 0xF4u) {
        seqLen = 4; hi2 = 0x8Fu; acc = b0 & 0x07u;
    }
    if (seqLen == 0) {
        *adv = 1; return 0;
    }
    for (k = 1; k < seqLen; k++) {
        unsigned int lo = (k == 1) ? lo2 : 0x80u;
        unsigned int hi = (k == 1) ? hi2 : 0xBFu;
        unsigned int bc;
        if (i + k >= n) { *adv = k; return 0; }
        bc = bytes[i + k];
        if (bc < lo || bc > hi) { *adv = k; return 0; }
        acc = (acc << 6) | (unsigned long)(bc & 0x3Fu);
    }
    *adv = seqLen; *cp = acc; return 1;
}

/* utf8_well_formed - 1 iff buf[0..n) is entirely well-formed UTF-8. */
static int utf8_well_formed(const unsigned char *buf, int n)
{
    int i = 0;
    while (i < n) {
        int adv = 0;
        unsigned long cp = 0;
        if (!oracle_step(buf, n, i, &adv, &cp)) {
            return 0;
        }
        i += adv;
    }
    return 1;
}

/* gen_wf - generate a well-formed UTF-16 unit sequence into u (astrals as
 * surrogate pairs); returns the unit count. */
static int gen_wf(prop_ctx *_pc, unsigned short *u)
{
    int ncp = PROP_INT(0, MAX_CP);
    int n = 0;
    int k;
    for (k = 0; k < ncp; k++) {
        long cp = (long)PROP_INT(0, 0x10FFFF);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0x41;     /* a surrogate scalar -> 'A' (well-formed) */
        }
        if (cp <= 0xFFFF) {
            u[n++] = (unsigned short)cp;
        } else {
            long v = cp - 0x10000L;
            u[n++] = (unsigned short)(0xD800L + (v >> 10));
            u[n++] = (unsigned short)(0xDC00L + (v & 0x3FFL));
        }
    }
    return n;
}

/* Property: round-trip on the well-formed UTF-16 subset is the identity. */
PROP_TEST(codec_round_trip) {
    unsigned short u[MAX_U];
    unsigned char out8[MAX_B];
    unsigned short back[MAX_U];
    EncStatus s1, s2;
    int n, nb, nu, k;

    n = gen_wf(_pc, u);
    s1 = (EncStatus)999;
    nb = Utf16ToUtf8(u, n, out8, (int)sizeof(out8), &s1);
    PROP_CHECK(s1 == ENC_OK);
    s2 = (EncStatus)999;
    nu = Utf8ToUtf16(out8, nb, back, MAX_U, &s2);
    PROP_CHECK(s2 == ENC_OK);
    PROP_CHECK(nu == n);
    for (k = 0; k < n; k++) {
        PROP_CHECK(back[k] == u[k]);
    }
}

/* Property: decode of arbitrary bytes is total and emits no lone surrogate. */
PROP_TEST(codec_decode_total) {
    unsigned char b[MAX_B];
    unsigned short out[MAX_B];
    EncStatus s;
    int n, nu, k, i, oracleUnits;

    n = PROP_INT(0, MAX_B);
    for (i = 0; i < n; i++) {
        b[i] = (unsigned char)PROP_INT(0, 255);
    }
    s = (EncStatus)999;
    nu = Utf8ToUtf16(b, n, out, MAX_B, &s);
    PROP_CHECK(nu >= 0 && nu <= MAX_B);

    /* Independent oracle: the codec consumed the whole input, producing exactly
     * the unit count a faithful Table 3-7 decoder would (ample buffer). */
    oracleUnits = 0;
    i = 0;
    while (i < n) {
        int adv = 0;
        unsigned long cp = 0;
        int wf = oracle_step(b, n, i, &adv, &cp);
        PROP_CHECK(adv > 0);
        if (!wf) {
            oracleUnits += 1;
        } else if (cp <= 0xFFFF) {
            oracleUnits += 1;
        } else {
            oracleUnits += 2;
        }
        i += adv;
    }
    PROP_CHECK(nu == oracleUnits);

    /* No lone surrogate is ever emitted. */
    for (k = 0; k < nu; k++) {
        if (out[k] >= 0xD800 && out[k] <= 0xDBFF) {
            PROP_CHECK(k + 1 < nu &&
                       out[k + 1] >= 0xDC00 && out[k + 1] <= 0xDFFF);
            k++;
        } else {
            PROP_CHECK(!(out[k] >= 0xDC00 && out[k] <= 0xDFFF));
        }
    }
}

/* Property: encode of arbitrary units always yields well-formed UTF-8. */
PROP_TEST(codec_never_invalid) {
    unsigned short u[MAX_CP];
    unsigned char out[MAX_B];
    EncStatus s;
    int n, nb, i;

    n = PROP_INT(0, MAX_CP);
    for (i = 0; i < n; i++) {
        u[i] = (unsigned short)PROP_INT(0, 0xFFFF);
    }
    s = (EncStatus)999;
    nb = Utf16ToUtf8(u, n, out, (int)sizeof(out), &s);
    PROP_CHECK(nb >= 0 && nb <= (int)sizeof(out));
    PROP_CHECK(utf8_well_formed(out, nb));
}

/* Property: a capped encode/decode leaves a whole valid prefix. */
PROP_TEST(codec_truncation_clean) {
    unsigned short u[MAX_U];
    unsigned char capped[MAX_B];
    unsigned char enc[MAX_B];
    unsigned short dec[MAX_U];
    EncStatus s;
    int n, cap, ncapped, nb, ucap, nu, k;

    n = gen_wf(_pc, u);

    /* encode direction: cap the byte output. */
    cap = PROP_INT(0, MAX_B);
    s = (EncStatus)999;
    ncapped = Utf16ToUtf8(u, n, capped, cap, &s);
    PROP_CHECK(ncapped >= 0 && ncapped <= cap);
    PROP_CHECK(s == ENC_OK || s == ENC_TRUNCATED);
    PROP_CHECK(utf8_well_formed(capped, ncapped));

    /* decode direction: cap the unit output. */
    s = (EncStatus)999;
    nb = Utf16ToUtf8(u, n, enc, (int)sizeof(enc), &s);
    ucap = PROP_INT(0, MAX_U);
    s = (EncStatus)999;
    nu = Utf8ToUtf16(enc, nb, dec, ucap, &s);
    PROP_CHECK(nu >= 0 && nu <= ucap);
    PROP_CHECK(s == ENC_OK || s == ENC_TRUNCATED);
    /* No trailing lone high surrogate; no lone surrogate anywhere. */
    if (nu > 0) {
        PROP_CHECK(!(dec[nu - 1] >= 0xD800 && dec[nu - 1] <= 0xDBFF));
    }
    for (k = 0; k < nu; k++) {
        if (dec[k] >= 0xD800 && dec[k] <= 0xDBFF) {
            k++;
        } else {
            PROP_CHECK(!(dec[k] >= 0xDC00 && dec[k] <= 0xDFFF));
        }
    }
}

int main(void)
{
    prop_seed(0);

    PROP_RUN(codec_round_trip,       2000);
    PROP_RUN(codec_decode_total,     2000);
    PROP_RUN(codec_never_invalid,    2000);
    PROP_RUN(codec_truncation_clean, 2000);

    return prop_summary();
}
