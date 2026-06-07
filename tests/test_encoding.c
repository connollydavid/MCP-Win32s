/*
 * test_encoding.c - on-target tests for the full-range text encoding subsystem
 * (work-item 5.4). Spec: encoding.allium.
 *
 * This file grows across the encoding modules; the section below covers the
 * code-page tables (M2): the exhaustive per-page round-trip on the bijective
 * subset (CodepageRoundTripsOnBijectiveSubset), the documented non-bijection
 * cases as named assertions, the reference mappings, and the strict-narrowing
 * reject (StrictNarrowingRejectsUnrepresentable). Obligations:
 * tests/OBLIGATIONS-5.4.md (table_round_trip_<cp>, nonbijection_*,
 * strict_narrowing_rejects, contract-signature.CodepageTables.*).
 *
 * Pure (no Win32): the tables are OS-independent, so this runs natively, under
 * Wine, and on real hardware identically.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#include "test_framework.h"
#include "charset_tables.h"
#include "encoding.h"

static const int SBCS_PAGES[] = {
    1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 874,
    437, 737, 775, 850, 852, 855, 857, 858, 860, 861, 862, 863, 864,
    865, 866, 869
};
#define N_SBCS ((int)(sizeof(SBCS_PAGES) / sizeof(SBCS_PAGES[0])))

static const int DBCS_PAGES[] = { 932, 936, 949, 950 };
#define N_DBCS ((int)(sizeof(DBCS_PAGES) / sizeof(DBCS_PAGES[0])))

/* Decode one 1- or 2-byte sequence to a single unit; -1 if undefined/lossy,
 * -2 on a length/consume surprise. */
static int dec1(int cp, const unsigned char *mb, int n)
{
    unsigned short u[4];
    int lossy = 0, consumed = 0, got;
    got = CpDecode(cp, mb, n, u, 4, &lossy, &consumed);
    if (got != 1 || lossy) {
        return -1;
    }
    if (consumed != n) {
        return -2;
    }
    return (int)u[0];
}

/* Discover the lead bytes of a DBCS page by probing CpDecode: a lead alone
 * decodes lossy (one U+FFFD); a lead + some valid trail does not. */
static int discover_leads(int cp, int *leads)
{
    int b, t, n = 0;
    for (b = 0x81; b <= 0xFE; b++) {
        unsigned char one[1];
        unsigned short u[2];
        int lossy = 0, cons = 0, got, found = 0;
        one[0] = (unsigned char)b;
        got = CpDecode(cp, one, 1, u, 2, &lossy, &cons);
        if (!(got == 1 && lossy)) {
            continue;
        }
        for (t = 0x00; t <= 0xFF; t++) {
            unsigned char two[2];
            int l2 = 0, c2 = 0, g2;
            two[0] = (unsigned char)b;
            two[1] = (unsigned char)t;
            g2 = CpDecode(cp, two, 2, u, 2, &l2, &c2);
            if (g2 == 1 && !l2 && c2 == 2) { found = 1; break; }
        }
        if (found) { leads[n++] = b; }
    }
    return n;
}

/* The bijective-subset round-trip property for one defined byte sequence:
 * encode(decode(B)) is representable and round-decodes to the same unit, and
 * the canonical (lowest) sequence re-encodes to itself. Returns 1 if B is in
 * the bijective subset (encode(decode(B)) == B), 0 if it is a documented
 * duplicate row, -1 on a failure (sets *why). */
static int roundtrip_one(int cp, const unsigned char *in, int n, const char **why)
{
    unsigned short u1[1];
    unsigned char out[2];
    int u, ra, en;

    u = dec1(cp, in, n);
    if (u < 0) { *why = "decode not total / single unit"; return -1; }
    u1[0] = (unsigned short)u;
    en = CpEncode(cp, u1, 1, out, 2, &ra);
    if (en <= 0) { *why = "decoded unit not representable on re-encode"; return -1; }
    if (dec1(cp, out, en) != u) { *why = "re-encode does not round-decode"; return -1; }
    if (en == n) {
        int i, same = 1;
        for (i = 0; i < n; i++) { if (out[i] != in[i]) { same = 0; break; } }
        if (same) { return 1; }    /* bijective */
    }
    return 0;                      /* documented duplicate */
}

TEST_CASE(cp_supported_set)
{
    int i;
    for (i = 0; i < N_SBCS; i++) {
        TEST_ASSERT(CpSupported(SBCS_PAGES[i]), "SBCS page supported");
    }
    for (i = 0; i < N_DBCS; i++) {
        TEST_ASSERT(CpSupported(DBCS_PAGES[i]), "DBCS page supported");
    }
    TEST_ASSERT(!CpSupported(9999), "unknown page unsupported");
    TEST_ASSERT(!CpSupported(65001), "UTF-8 not a baked codepage table");
}

TEST_CASE(sbcs_round_trip_exhaustive)
{
    int i, b;
    for (i = 0; i < N_SBCS; i++) {
        int cp = SBCS_PAGES[i];
        long bij = 0;
        for (b = 0; b < 256; b++) {
            unsigned char in[1];
            const char *why = "";
            int r;
            in[0] = (unsigned char)b;
            if (dec1(cp, in, 1) < 0) {
                continue;          /* undefined -> covered by the FFFD test */
            }
            r = roundtrip_one(cp, in, 1, &why);
            TEST_ASSERT(r >= 0, why);
            if (r == 1) { bij++; }
        }
        TEST_ASSERT(bij > 0, "page has a non-empty bijective subset");
    }
}

TEST_CASE(dbcs_round_trip_exhaustive)
{
    int i;
    static int leads[256];
    for (i = 0; i < N_DBCS; i++) {
        int cp = DBCS_PAGES[i];
        int nleads, li, b, t;
        long bij = 0;
        /* single (non-lead) bytes */
        nleads = discover_leads(cp, leads);
        for (b = 0; b < 256; b++) {
            unsigned char in[1];
            const char *why = "";
            int isLead = 0, r;
            for (li = 0; li < nleads; li++) { if (leads[li] == b) { isLead = 1; break; } }
            if (isLead) { continue; }
            in[0] = (unsigned char)b;
            if (dec1(cp, in, 1) < 0) { continue; }
            r = roundtrip_one(cp, in, 1, &why);
            TEST_ASSERT(r >= 0, why);
            if (r == 1) { bij++; }
        }
        /* every lead/trail pair */
        for (li = 0; li < nleads; li++) {
            for (t = 0; t < 256; t++) {
                unsigned char in[2];
                const char *why = "";
                int r;
                in[0] = (unsigned char)leads[li];
                in[1] = (unsigned char)t;
                if (dec1(cp, in, 2) < 0) { continue; }
                r = roundtrip_one(cp, in, 2, &why);
                TEST_ASSERT(r >= 0, why);
                if (r == 1) { bij++; }
            }
        }
        TEST_ASSERT(bij > 0, "DBCS page has a non-empty bijective subset");
    }
}

TEST_CASE(sbcs_undefined_decodes_replacement)
{
    /* The decode is total: an undefined position yields exactly one U+FFFD,
     * marked lossy (never a failure). cp1253 has documented holes. */
    unsigned char in[1];
    unsigned short u[2];
    int lossy, consumed, got;
    in[0] = 0xAA;              /* cp1253 0xAA is undefined */
    lossy = 0; consumed = 0;
    got = CpDecode(1253, in, 1, u, 2, &lossy, &consumed);
    TEST_ASSERT(got == 1 && consumed == 1, "undefined byte still produces one unit");
    TEST_ASSERT(u[0] == 0xFFFD && lossy == 1, "undefined byte decodes to U+FFFD, lossy");
}

TEST_CASE(nonbijection_cp1252_undefined)
{
    /* The five documented undefined cp1252 positions decode to U+FFFD. */
    static const unsigned char undef[5] = { 0x81, 0x8D, 0x8F, 0x90, 0x9D };
    int i;
    for (i = 0; i < 5; i++) {
        TEST_ASSERT(dec1(1252, &undef[i], 1) < 0, "cp1252 undefined -> U+FFFD");
    }
    /* a defined neighbour still maps (0x80 -> Euro) */
    {
        unsigned char b = 0x80;
        TEST_ASSERT(dec1(1252, &b, 1) == 0x20AC, "cp1252 0x80 -> U+20AC");
    }
}

TEST_CASE(nonbijection_cp932_wave_dash)
{
    /* The Microsoft-canonical wave-dash / fullwidth-tilde choice: cp932 0x8160
     * decodes to U+FF5E (FULLWIDTH TILDE), NOT JIS U+301C. The device applies
     * the canonical mapping as-is (a recorded non-obligation: this asserts the
     * mapping is APPLIED, not that it is the "right" Unicode). */
    unsigned char wd[2];
    unsigned short u[1];
    int ra, en;
    wd[0] = 0x81; wd[1] = 0x60;
    TEST_ASSERT(dec1(932, wd, 2) == 0xFF5E, "cp932 0x8160 -> U+FF5E (canonical)");
    /* and it re-encodes back to 0x8160 (round-trips on the canonical side) */
    u[0] = 0xFF5E;
    en = CpEncode(932, u, 1, wd, 2, &ra);
    TEST_ASSERT(en == 2 && wd[0] == 0x81 && wd[1] == 0x60, "U+FF5E -> cp932 0x8160");
}

TEST_CASE(dbcs_reference_values)
{
    unsigned char mb[2];
    mb[0] = 0x81; mb[1] = 0x40;
    TEST_ASSERT(dec1(932, mb, 2) == 0x3000, "cp932 0x8140 -> U+3000");
    mb[0] = 0xB0; mb[1] = 0xA1;
    TEST_ASSERT(dec1(936, mb, 2) == 0x554A, "cp936 0xB0A1 -> U+554A");
    mb[0] = 0xB0; mb[1] = 0xA1;
    TEST_ASSERT(dec1(949, mb, 2) == 0xAC00, "cp949 0xB0A1 -> U+AC00");
    mb[0] = 0xA4; mb[1] = 0x40;
    TEST_ASSERT(dec1(950, mb, 2) == 0x4E00, "cp950 0xA440 -> U+4E00");
}

TEST_CASE(cp858_derived_from_cp850)
{
    /* cp858 = cp850 with the single Euro override at 0xD5. */
    unsigned char b;
    b = 0xD5;
    TEST_ASSERT(dec1(850, &b, 1) == 0x0131, "cp850 0xD5 -> U+0131 (dotless i)");
    TEST_ASSERT(dec1(858, &b, 1) == 0x20AC, "cp858 0xD5 -> U+20AC (Euro)");
    /* every other position matches cp850 (spot 0xD4) */
    b = 0xD4;
    TEST_ASSERT(dec1(858, &b, 1) == dec1(850, &b, 1), "cp858 == cp850 at 0xD4");
}

TEST_CASE(strict_narrowing_rejects_unrepresentable)
{
    /* A code unit the target page cannot represent is REJECTED - no '?'
     * substitution, *rejectAt set (StrictNarrowingRejectsUnrepresentable). */
    unsigned short u[2];
    unsigned char out[4];
    int ra, en;
    u[0] = 0x4E00;            /* CJK ideograph: not in cp1252 */
    ra = -999;
    en = CpEncode(1252, u, 1, out, 4, &ra);
    TEST_ASSERT(en == -1 && ra == 0, "cp1252 rejects U+4E00 at index 0");
    TEST_ASSERT(CpRepresentable(1252, u, 1) == 0, "U+4E00 not representable in cp1252");
    /* the offending unit is reported even mid-sequence */
    u[0] = 0x41; u[1] = 0x4E00;
    ra = -999;
    en = CpEncode(1252, u, 2, out, 4, &ra);
    TEST_ASSERT(en == -1 && ra == 1, "reject reports the offending unit index");
}

TEST_CASE(lone_surrogate_rejected)
{
    /* A lone surrogate is representable in no baked page. */
    unsigned short u[1];
    unsigned char out[4];
    int ra;
    u[0] = 0xD800;
    TEST_ASSERT(CpEncode(932, u, 1, out, 4, &ra) == -1, "cp932 rejects lone surrogate");
    TEST_ASSERT(CpRepresentable(1252, u, 1) == 0, "lone surrogate not representable");
}

TEST_CASE(representable_predicate)
{
    unsigned short u[2];
    u[0] = 0x41;             /* 'A' representable everywhere */
    TEST_ASSERT(CpRepresentable(1252, u, 1) == 1, "'A' representable in cp1252");
    TEST_ASSERT(CpRepresentable(932, u, 1) == 1, "'A' representable in cp932");
    u[0] = 0x20AC;           /* Euro: in cp1252, not in cp437 */
    TEST_ASSERT(CpRepresentable(1252, u, 1) == 1, "Euro representable in cp1252");
    TEST_ASSERT(CpRepresentable(437, u, 1) == 0, "Euro not representable in cp437");
}

/* ------------------------------------------------------------------ */
/* The pure UTF-8 <-> UTF-16 codec (encoding.h Utf8Codec contract).    */
/* ------------------------------------------------------------------ */

/* utf8_valid - 1 iff buf[0..n) is entirely well-formed UTF-8 (Table 3-7),
 * checked independently of the codec, used to confirm an encode's output. */
static int utf8_valid(const unsigned char *buf, int n)
{
    int i = 0;
    while (i < n) {
        unsigned int b0 = buf[i];
        int          seqLen, k;
        unsigned int lo2, hi2;
        if (b0 <= 0x7Fu) { i += 1; continue; }
        seqLen = 0; lo2 = 0x80u; hi2 = 0xBFu;
        if (b0 >= 0xC2u && b0 <= 0xDFu) { seqLen = 2; }
        else if (b0 == 0xE0u) { seqLen = 3; lo2 = 0xA0u; }
        else if (b0 >= 0xE1u && b0 <= 0xECu) { seqLen = 3; }
        else if (b0 == 0xEDu) { seqLen = 3; hi2 = 0x9Fu; }
        else if (b0 >= 0xEEu && b0 <= 0xEFu) { seqLen = 3; }
        else if (b0 == 0xF0u) { seqLen = 4; lo2 = 0x90u; }
        else if (b0 >= 0xF1u && b0 <= 0xF3u) { seqLen = 4; }
        else if (b0 == 0xF4u) { seqLen = 4; hi2 = 0x8Fu; }
        if (seqLen == 0) { return 0; }
        for (k = 1; k < seqLen; k++) {
            unsigned int lo = (k == 1) ? lo2 : 0x80u;
            unsigned int hi = (k == 1) ? hi2 : 0xBFu;
            unsigned int bc;
            if (i + k >= n) { return 0; }
            bc = buf[i + k];
            if (bc < lo || bc > hi) { return 0; }
        }
        i += seqLen;
    }
    return 1;
}

TEST_CASE(codec_round_trip_ascii_bmp_astral)
{
    /* 'A' (1 byte), U+00E9 e-acute (2), U+4E2D (3), U+1F600 (astral, the pair
     * D83D DE00) all round-trip identically through encode then decode. */
    unsigned short u[5];
    unsigned char out8[16];
    unsigned short back[8];
    EncStatus s;
    int nb, nu, i;

    u[0] = 0x0041;          /* 'A' */
    u[1] = 0x00E9;          /* e-acute */
    u[2] = 0x4E2D;          /* CJK */
    u[3] = 0xD83D;          /* astral high surrogate */
    u[4] = 0xDE00;          /* astral low surrogate */

    s = (EncStatus)999;
    nb = Utf16ToUtf8(u, 5, out8, (int)sizeof(out8), &s);
    TEST_ASSERT_INT_EQUAL(ENC_OK, s, "encode status OK");
    TEST_ASSERT_INT_EQUAL(1 + 2 + 3 + 4, nb, "encoded byte count 1+2+3+4");
    TEST_ASSERT(utf8_valid(out8, nb), "encoded output is well-formed UTF-8");

    s = (EncStatus)999;
    nu = Utf8ToUtf16(out8, nb, back, 8, &s);
    TEST_ASSERT_INT_EQUAL(ENC_OK, s, "decode status OK");
    TEST_ASSERT_INT_EQUAL(5, nu, "round-trips to 5 units");
    for (i = 0; i < 5; i++) {
        TEST_ASSERT(back[i] == u[i], "unit round-trips identically");
    }
}

TEST_CASE(codec_lone_surrogate_becomes_replacement)
{
    /* A lone high surrogate (0xD800) and a lone low surrogate (0xDC00) each
     * encode to EF BF BD (U+FFFD), status ENC_LOSSY; output is valid UTF-8. */
    unsigned short u[1];
    unsigned char out8[8];
    EncStatus s;
    int nb;

    u[0] = 0xD800;          /* lone high surrogate */
    s = (EncStatus)999;
    nb = Utf16ToUtf8(u, 1, out8, (int)sizeof(out8), &s);
    TEST_ASSERT_INT_EQUAL(ENC_LOSSY, s, "lone high surrogate is lossy");
    TEST_ASSERT_INT_EQUAL(3, nb, "U+FFFD is 3 bytes");
    TEST_ASSERT(out8[0] == 0xEF && out8[1] == 0xBF && out8[2] == 0xBD,
                "lone high surrogate -> EF BF BD");
    TEST_ASSERT(utf8_valid(out8, nb), "output is well-formed UTF-8");

    u[0] = 0xDC00;          /* lone low surrogate */
    s = (EncStatus)999;
    nb = Utf16ToUtf8(u, 1, out8, (int)sizeof(out8), &s);
    TEST_ASSERT_INT_EQUAL(ENC_LOSSY, s, "lone low surrogate is lossy");
    TEST_ASSERT_INT_EQUAL(3, nb, "U+FFFD is 3 bytes");
    TEST_ASSERT(out8[0] == 0xEF && out8[1] == 0xBF && out8[2] == 0xBD,
                "lone low surrogate -> EF BF BD");
    TEST_ASSERT(utf8_valid(out8, nb), "output is well-formed UTF-8");
}

TEST_CASE(codec_decode_total_on_garbage)
{
    /* Maximal-subpart fixtures: an ill-formed sequence emits ONE U+FFFD for its
     * maximal valid subpart, then resyncs AT the first non-permitted byte. */
    unsigned char in[8];
    unsigned short out[8];
    EncStatus s;
    int nu;

    /* {E0,41}: E0 needs A0..BF next; 0x41 is not -> E0 alone is the maximal
     * subpart (one U+FFFD), resync AT 0x41 -> 'A'. */
    in[0] = 0xE0; in[1] = 0x41;
    s = (EncStatus)999;
    nu = Utf8ToUtf16(in, 2, out, 8, &s);
    TEST_ASSERT_INT_EQUAL(ENC_LOSSY, s, "{E0,41} is lossy");
    TEST_ASSERT_INT_EQUAL(2, nu, "{E0,41} -> two units");
    TEST_ASSERT(out[0] == 0xFFFD && out[1] == 0x0041, "{E0,41} -> FFFD,'A'");

    /* {ED,A0,80} (a CESU surrogate): ED permits only 80..9F next; A0 stops it
     * -> one U+FFFD at ED; then A0 is a stray continuation -> U+FFFD; then 80
     * is a stray continuation -> U+FFFD. */
    in[0] = 0xED; in[1] = 0xA0; in[2] = 0x80;
    s = (EncStatus)999;
    nu = Utf8ToUtf16(in, 3, out, 8, &s);
    TEST_ASSERT_INT_EQUAL(ENC_LOSSY, s, "{ED,A0,80} is lossy");
    TEST_ASSERT_INT_EQUAL(3, nu, "{ED,A0,80} -> three units");
    TEST_ASSERT(out[0] == 0xFFFD && out[1] == 0xFFFD && out[2] == 0xFFFD,
                "{ED,A0,80} -> FFFD,FFFD,FFFD");

    /* {80}: a lone continuation byte -> one U+FFFD. */
    in[0] = 0x80;
    s = (EncStatus)999;
    nu = Utf8ToUtf16(in, 1, out, 8, &s);
    TEST_ASSERT_INT_EQUAL(ENC_LOSSY, s, "{80} is lossy");
    TEST_ASSERT_INT_EQUAL(1, nu, "{80} -> one unit");
    TEST_ASSERT(out[0] == 0xFFFD, "{80} -> FFFD");

    /* {F0,28,8C,28}: F0 needs 90..BF next; 0x28 is not -> F0 alone (one U+FFFD),
     * resync AT 0x28 -> '('; then 8C stray -> U+FFFD; then 0x28 -> '('. */
    in[0] = 0xF0; in[1] = 0x28; in[2] = 0x8C; in[3] = 0x28;
    s = (EncStatus)999;
    nu = Utf8ToUtf16(in, 4, out, 8, &s);
    TEST_ASSERT_INT_EQUAL(ENC_LOSSY, s, "{F0,28,8C,28} is lossy");
    TEST_ASSERT_INT_EQUAL(4, nu, "{F0,28,8C,28} -> four units");
    TEST_ASSERT(out[0] == 0xFFFD && out[1] == 0x0028 &&
                out[2] == 0xFFFD && out[3] == 0x0028,
                "{F0,28,8C,28} -> FFFD,'(',FFFD,'('");
}

TEST_CASE(codec_truncation_boundary_clean)
{
    /* Encode two U+4E2D (3 bytes each) into a 4-byte buffer: only the first
     * char fits, the encode stops on a code-point boundary (ENC_TRUNCATED). */
    unsigned short u[2];
    unsigned char out8[4];
    EncStatus s;
    int nb;

    u[0] = 0x4E2D; u[1] = 0x4E2D;
    s = (EncStatus)999;
    nb = Utf16ToUtf8(u, 2, out8, 4, &s);
    TEST_ASSERT_INT_EQUAL(ENC_TRUNCATED, s, "encode hits the 4-byte bound");
    TEST_ASSERT_INT_EQUAL(3, nb, "writes one whole 3-byte char, not four");
    TEST_ASSERT(utf8_valid(out8, nb), "truncated prefix is whole valid UTF-8");

    /* Decode a 4-byte astral char (U+1F600 -> F0 9F 98 80) into a 1-unit
     * buffer: the surrogate pair will not fit, so 0 units are written and the
     * decode stops cleanly (never a lone high surrogate). */
    {
        unsigned char astral[4];
        unsigned short dec[2];
        int nu;
        astral[0] = 0xF0; astral[1] = 0x9F; astral[2] = 0x98; astral[3] = 0x80;
        s = (EncStatus)999;
        nu = Utf8ToUtf16(astral, 4, dec, 1, &s);
        TEST_ASSERT_INT_EQUAL(ENC_TRUNCATED, s, "decode won't split the pair");
        TEST_ASSERT_INT_EQUAL(0, nu, "no lone high surrogate written");
    }
}

int main(void)
{
    printf("test_encoding (charset tables - M2):\n");
    RUN_TEST(cp_supported_set);
    RUN_TEST(sbcs_round_trip_exhaustive);
    RUN_TEST(dbcs_round_trip_exhaustive);
    RUN_TEST(sbcs_undefined_decodes_replacement);
    RUN_TEST(nonbijection_cp1252_undefined);
    RUN_TEST(nonbijection_cp932_wave_dash);
    RUN_TEST(dbcs_reference_values);
    RUN_TEST(cp858_derived_from_cp850);
    RUN_TEST(strict_narrowing_rejects_unrepresentable);
    RUN_TEST(lone_surrogate_rejected);
    RUN_TEST(representable_predicate);
    RUN_TEST(codec_round_trip_ascii_bmp_astral);
    RUN_TEST(codec_lone_surrogate_becomes_replacement);
    RUN_TEST(codec_decode_total_on_garbage);
    RUN_TEST(codec_truncation_boundary_clean);
    print_test_summary();
    return g_tests_failed;
}
