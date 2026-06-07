/*
 * encoding.c - Full-range text encoding for MCP-Win32s (work-item 5.4).
 *
 * This translation unit carries the PURE UTF-8 <-> UTF-16 codec (the
 * Utf8Codec contract: utf16_to_utf8 / utf8_to_utf16). No Win32, no floating
 * point, no CP_UTF8 dependency - it compiles unchanged as a target TU (where
 * encoding.h pulls in windows.h) and host-pure (gcc -DENCODING_HOST_PURE).
 *
 * The two SAFETY pins the spec (encoding.allium) names on this codec:
 *   NeverEmitInvalidUtf8 - an unpaired UTF-16 surrogate is emitted as U+FFFD,
 *     never a CESU-8/WTF-8 sequence, so every byte the encoder writes is
 *     well-formed UTF-8.
 *   LossyDecodeIsTotal - the decoder never fails, crashes or loops on
 *     ARBITRARY bytes: an ill-formed sequence yields exactly one U+FFFD for its
 *     maximal valid subpart (Unicode Standard Table 3-7), then the scan resumes
 *     at the first non-permitted byte, so it always makes progress.
 *   TruncationIsBoundaryClean - both directions stop on a code-point / code-unit
 *     boundary (never a partial sequence or a split surrogate pair) when the
 *     output buffer is reached, reporting ENC_TRUNCATED.
 *
 * Hard constraints (this binary's own source, per CLAUDE.md): C89, declarations
 * at block top, slash-star comments only, no floating point, i386, no threads.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#include "encoding.h"

/*
 * Utf16ToUtf8 - Encode UTF-16 code units to UTF-8 (spec: utf16_to_utf8).
 * See the header for the full contract.
 */
int Utf16ToUtf8(const unsigned short *units, int nUnits,
                unsigned char *out, int outCap, EncStatus *status)
{
    int            i;
    int            n;
    EncStatus      st;
    unsigned long  cp;

    n = 0;
    st = ENC_OK;
    i = 0;
    while (i < nUnits) {
        unsigned int u = units[i];
        int          len;

        if (u >= 0xD800u && u <= 0xDBFFu) {
            /* High surrogate: pair it with a following low surrogate, else
             * substitute U+FFFD (NeverEmitInvalidUtf8). */
            if (i + 1 < nUnits &&
                units[i + 1] >= 0xDC00u && units[i + 1] <= 0xDFFFu) {
                unsigned int lo = units[i + 1];
                cp = 0x10000UL +
                     (((unsigned long)(u - 0xD800u)) << 10) +
                     (unsigned long)(lo - 0xDC00u);
                i += 2;
            } else {
                cp = (unsigned long)CP_REPLACEMENT;
                st = ENC_LOSSY;
                i += 1;
            }
        } else if (u >= 0xDC00u && u <= 0xDFFFu) {
            /* Unpaired low surrogate: substitute U+FFFD. */
            cp = (unsigned long)CP_REPLACEMENT;
            st = ENC_LOSSY;
            i += 1;
        } else {
            cp = (unsigned long)u;
            i += 1;
        }

        /* Canonical UTF-8 length for this code point (no overlong forms). */
        if (cp <= 0x7FUL) {
            len = 1;
        } else if (cp <= 0x7FFUL) {
            len = 2;
        } else if (cp <= 0xFFFFUL) {
            len = 3;
        } else {
            len = 4;
        }

        /* TruncationIsBoundaryClean: the whole sequence must fit, or stop. */
        if (n + len > outCap) {
            st = ENC_TRUNCATED;
            break;
        }

        switch (len) {
        case 1:
            out[n++] = (unsigned char)cp;
            break;
        case 2:
            out[n++] = (unsigned char)(0xC0UL | (cp >> 6));
            out[n++] = (unsigned char)(0x80UL | (cp & 0x3FUL));
            break;
        case 3:
            out[n++] = (unsigned char)(0xE0UL | (cp >> 12));
            out[n++] = (unsigned char)(0x80UL | ((cp >> 6) & 0x3FUL));
            out[n++] = (unsigned char)(0x80UL | (cp & 0x3FUL));
            break;
        default: /* 4 */
            out[n++] = (unsigned char)(0xF0UL | (cp >> 18));
            out[n++] = (unsigned char)(0x80UL | ((cp >> 12) & 0x3FUL));
            out[n++] = (unsigned char)(0x80UL | ((cp >> 6) & 0x3FUL));
            out[n++] = (unsigned char)(0x80UL | (cp & 0x3FUL));
            break;
        }
    }

    if (status != 0) {
        *status = st;
    }
    return n;
}

/*
 * Utf8ToUtf16 - Decode UTF-8 bytes to UTF-16 code units (spec: utf8_to_utf16).
 * See the header for the full contract.
 *
 * Validity oracle: Unicode Standard Table 3-7 "Well-Formed UTF-8 Byte
 * Sequences". For each lead byte the second byte's permitted range narrows
 * (E0/ED/F0/F4 are the special rows that exclude overlongs and surrogates and
 * cap at U+10FFFF); the third and fourth continuation bytes are always
 * 0x80..0xBF. On an ill-formed sequence we emit ONE U+FFFD for the maximal
 * valid subpart and resync AT the first non-permitted byte (never consuming
 * it), which is what guarantees forward progress on arbitrary input.
 */
int Utf8ToUtf16(const unsigned char *bytes, int nBytes,
                unsigned short *out, int outCap, EncStatus *status)
{
    int       i;
    int       n;
    EncStatus st;

    n = 0;
    st = ENC_OK;
    i = 0;
    while (i < nBytes) {
        unsigned int  b0 = bytes[i];
        int           seqLen;       /* total bytes in a well-formed sequence */
        unsigned int  lo2;          /* second byte's permitted low bound */
        unsigned int  hi2;          /* second byte's permitted high bound */
        unsigned long cp;
        int           bad;
        int           k;

        if (b0 <= 0x7Fu) {
            /* 00..7F: one byte, no continuation. */
            if (n + 1 > outCap) {
                st = ENC_TRUNCATED;
                break;
            }
            out[n++] = (unsigned short)b0;
            i += 1;
            continue;
        }

        /* Classify the lead byte per Table 3-7. seqLen 0 marks an illegal lead
         * (a stray continuation 80..BF, or C0/C1/F5..FF): a length-1 maximal
         * subpart -> one U+FFFD, advance 1. */
        seqLen = 0;
        lo2 = 0x80u;
        hi2 = 0xBFu;
        cp = 0;
        if (b0 >= 0xC2u && b0 <= 0xDFu) {
            seqLen = 2;
            cp = b0 & 0x1Fu;
        } else if (b0 == 0xE0u) {
            seqLen = 3; lo2 = 0xA0u; hi2 = 0xBFu; cp = b0 & 0x0Fu;
        } else if (b0 >= 0xE1u && b0 <= 0xECu) {
            seqLen = 3; cp = b0 & 0x0Fu;
        } else if (b0 == 0xEDu) {
            seqLen = 3; lo2 = 0x80u; hi2 = 0x9Fu; cp = b0 & 0x0Fu;
        } else if (b0 >= 0xEEu && b0 <= 0xEFu) {
            seqLen = 3; cp = b0 & 0x0Fu;
        } else if (b0 == 0xF0u) {
            seqLen = 4; lo2 = 0x90u; hi2 = 0xBFu; cp = b0 & 0x07u;
        } else if (b0 >= 0xF1u && b0 <= 0xF3u) {
            seqLen = 4; cp = b0 & 0x07u;
        } else if (b0 == 0xF4u) {
            seqLen = 4; lo2 = 0x80u; hi2 = 0x8Fu; cp = b0 & 0x07u;
        }

        if (seqLen == 0) {
            /* Illegal lead: maximal subpart of length 1. */
            if (n + 1 > outCap) {
                st = ENC_TRUNCATED;
                break;
            }
            out[n++] = (unsigned short)CP_REPLACEMENT;
            st = ENC_LOSSY;
            i += 1;
            continue;
        }

        /* Consume continuation bytes ONLY while permitted. The second byte uses
         * the row-specific [lo2,hi2]; bytes 3 and 4 use the generic 80..BF.
         * `bad` records the position of the first non-permitted byte (or
         * end-of-input), which is where the scan must resync. */
        bad = 0;
        for (k = 1; k < seqLen; k++) {
            unsigned int lo = (k == 1) ? lo2 : 0x80u;
            unsigned int hi = (k == 1) ? hi2 : 0xBFu;
            unsigned int bc;
            if (i + k >= nBytes) {
                bad = 1;            /* truncated input: subpart ends here */
                break;
            }
            bc = bytes[i + k];
            if (bc < lo || bc > hi) {
                bad = 1;            /* this byte is not part of the sequence */
                break;
            }
            cp = (cp << 6) | (unsigned long)(bc & 0x3Fu);
        }

        if (bad) {
            /* Ill-formed: emit ONE U+FFFD for the k-byte maximal subpart and
             * resync AT byte i+k (do NOT consume the offending byte). */
            if (n + 1 > outCap) {
                st = ENC_TRUNCATED;
                break;
            }
            out[n++] = (unsigned short)CP_REPLACEMENT;
            st = ENC_LOSSY;
            i += k;
            continue;
        }

        /* Well-formed sequence: cp is the code point. */
        if (cp <= 0xFFFFUL) {
            if (n + 1 > outCap) {
                st = ENC_TRUNCATED;
                break;
            }
            out[n++] = (unsigned short)cp;
        } else {
            /* Astral: surrogate pair. TruncationIsBoundaryClean - never split
             * the pair, so two units must fit. */
            unsigned long v;
            if (n + 2 > outCap) {
                st = ENC_TRUNCATED;
                break;
            }
            v = cp - 0x10000UL;
            out[n++] = (unsigned short)(0xD800UL + (v >> 10));
            out[n++] = (unsigned short)(0xDC00UL + (v & 0x3FFUL));
        }
        i += seqLen;
    }

    if (status != 0) {
        *status = st;
    }
    return n;
}
