/*
 * theft_encoding.c - host-native property-based tests for the pure UTF-8 <->
 * UTF-16 codec (src/encoding.c: Utf16ToUtf8 / Utf8ToUtf16).
 *
 * Theft host PBT harness (specs/encoding.allium contract Utf8Codec). The
 * codec's SAFETY pins, each hammered at 50k autoshrinking trials:
 *
 *   codec/round_trip      a WELL-FORMED UTF-16 unit sequence survives
 *                         Utf8ToUtf16(Utf16ToUtf8(u)) == u exactly, ENC_OK both
 *                         ways (identity on the well-formed subset).
 *   codec/decode_total    LossyDecodeIsTotal: Utf8ToUtf16 on ARBITRARY bytes
 *                         never crashes/loops, writes <= outCap units, consumes
 *                         the WHOLE input (a byte-cursor oracle walks the same
 *                         Table 3-7 logic), and emits NO lone surrogate.
 *   codec/never_invalid   NeverEmitInvalidUtf8: every byte Utf16ToUtf8 writes
 *                         from ARBITRARY units is well-formed UTF-8 (an
 *                         independent in-test Table 3-7 validator finds zero
 *                         ill-formed sequences; every lone surrogate -> U+FFFD).
 *   codec/truncation_clean  TruncationIsBoundaryClean: a capped encode/decode
 *                         leaves a whole, valid prefix (no partial sequence, no
 *                         split surrogate pair); status is ENC_OK | ENC_TRUNCATED.
 *
 * Only the pure codec is exercised (no Win32). encoding.c references no Win32
 * type, so it is compiled here directly (its pure half is unconditional). This
 * harness is C99 + POSIX, built natively with gcc.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "theft.h"
#include "encoding.h"

#define TRIALS  50000
#define SEED    0x5E44CDEF0001ULL

/* ============================================================= */
/* Independent in-test oracle: walk Table 3-7 over a byte buffer */
/* ============================================================= */

/*
 * oracle_step - classify the sequence starting at bytes[i] of length n.
 *   *adv  receives the number of bytes the codec MUST advance.
 *   *cp   receives the decoded code point (only meaningful when the return is 1).
 * Returns 1 for a well-formed sequence, 0 for an ill-formed maximal subpart
 * (one U+FFFD, advance the subpart length). This is the same logic the codec
 * uses, written independently so the test does not merely echo the code.
 */
static int oracle_step(const uint8_t *bytes, int n, int i, int *adv,
                       uint32_t *cp)
{
    uint32_t b0 = bytes[i];
    int seqLen, k;
    uint32_t lo2, hi2, acc;

    if (b0 <= 0x7F) {
        *adv = 1;
        *cp = b0;
        return 1;
    }

    seqLen = 0;
    lo2 = 0x80;
    hi2 = 0xBF;
    acc = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        seqLen = 2; acc = b0 & 0x1F;
    } else if (b0 == 0xE0) {
        seqLen = 3; lo2 = 0xA0; acc = b0 & 0x0F;
    } else if (b0 >= 0xE1 && b0 <= 0xEC) {
        seqLen = 3; acc = b0 & 0x0F;
    } else if (b0 == 0xED) {
        seqLen = 3; hi2 = 0x9F; acc = b0 & 0x0F;
    } else if (b0 >= 0xEE && b0 <= 0xEF) {
        seqLen = 3; acc = b0 & 0x0F;
    } else if (b0 == 0xF0) {
        seqLen = 4; lo2 = 0x90; acc = b0 & 0x07;
    } else if (b0 >= 0xF1 && b0 <= 0xF3) {
        seqLen = 4; acc = b0 & 0x07;
    } else if (b0 == 0xF4) {
        seqLen = 4; hi2 = 0x8F; acc = b0 & 0x07;
    }

    if (seqLen == 0) {
        *adv = 1;            /* illegal lead: length-1 subpart */
        return 0;
    }

    for (k = 1; k < seqLen; k++) {
        uint32_t lo = (k == 1) ? lo2 : 0x80;
        uint32_t hi = (k == 1) ? hi2 : 0xBF;
        uint32_t bc;
        if (i + k >= n) { *adv = k; return 0; }
        bc = bytes[i + k];
        if (bc < lo || bc > hi) { *adv = k; return 0; }
        acc = (acc << 6) | (bc & 0x3F);
    }
    *adv = seqLen;
    *cp = acc;
    return 1;
}

/* is_lone_surrogate - 1 iff u is a UTF-16 surrogate code unit. */
static int is_surrogate(unsigned short u)
{
    return (u >= 0xD800 && u <= 0xDFFF) ? 1 : 0;
}

/* utf8_well_formed - 1 iff buf[0..n) is entirely well-formed UTF-8 (Table 3-7,
 * surrogates excluded), independent of the codec. */
static int utf8_well_formed(const uint8_t *buf, int n)
{
    int i = 0;
    while (i < n) {
        int adv = 0;
        uint32_t cp = 0;
        if (!oracle_step(buf, n, i, &adv, &cp)) {
            return 0;
        }
        i += adv;
    }
    return 1;
}

/* ============================================================= */
/* Generators                                                    */
/* ============================================================= */

#define MAX_CP  64      /* code points in a well-formed sample */
#define MAX_U   192     /* units it can expand to (worst case 2x + headroom) */
#define MAX_B   256     /* bytes a sample can expand to (worst case 4x) */

/* A well-formed UTF-16 unit sequence: code points in U+0000..U+10FFFF minus the
 * surrogate block, astrals stored as a surrogate pair. */
struct wf_units {
    unsigned short u[MAX_U];
    int            n;
};

static enum theft_alloc_res
wf_alloc(struct theft *t, void *env, void **out)
{
    struct wf_units *w;
    int ncp, k;
    (void)env;
    w = malloc(sizeof(*w));
    if (w == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    ncp = (int)(theft_random_bits(t, 6) % (MAX_CP + 1));   /* 0..MAX_CP */
    w->n = 0;
    for (k = 0; k < ncp; k++) {
        uint32_t cp = (uint32_t)theft_random_bits(t, 21) % 0x110000U;
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0x41;     /* replace a surrogate scalar with 'A' (well-formed) */
        }
        if (cp <= 0xFFFF) {
            w->u[w->n++] = (unsigned short)cp;
        } else {
            uint32_t v = cp - 0x10000U;
            w->u[w->n++] = (unsigned short)(0xD800 + (v >> 10));
            w->u[w->n++] = (unsigned short)(0xDC00 + (v & 0x3FF));
        }
    }
    *out = w;
    return THEFT_ALLOC_OK;
}

static theft_hash wf_hash(const void *instance, void *env)
{
    const struct wf_units *w = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)w->u,
                              (size_t)w->n * sizeof(w->u[0]));
}

static struct theft_type_info wf_info = {
    .alloc = wf_alloc,
    .free  = theft_generic_free_cb,
    .hash  = wf_hash,
};

/* Arbitrary bytes (any value, any length up to MAX_B). */
struct raw_bytes {
    uint8_t b[MAX_B];
    int     n;
};

static enum theft_alloc_res
raw_alloc(struct theft *t, void *env, void **out)
{
    struct raw_bytes *r;
    int k;
    (void)env;
    r = malloc(sizeof(*r));
    if (r == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    r->n = (int)(theft_random_bits(t, 6) % (MAX_B + 1));   /* 0..MAX_B */
    for (k = 0; k < r->n; k++) {
        r->b[k] = (uint8_t)theft_random_bits(t, 8);
    }
    *out = r;
    return THEFT_ALLOC_OK;
}

static theft_hash raw_hash(const void *instance, void *env)
{
    const struct raw_bytes *r = instance;
    (void)env;
    return theft_hash_onepass(r->b, (size_t)r->n);
}

static struct theft_type_info raw_info = {
    .alloc = raw_alloc,
    .free  = theft_generic_free_cb,
    .hash  = raw_hash,
};

/* Arbitrary UTF-16 units (any 0x0000..0xFFFF, so lone surrogates are common). */
struct arb_units {
    unsigned short u[MAX_CP];
    int            n;
};

static enum theft_alloc_res
arb_alloc(struct theft *t, void *env, void **out)
{
    struct arb_units *a;
    int k;
    (void)env;
    a = malloc(sizeof(*a));
    if (a == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    a->n = (int)(theft_random_bits(t, 6) % (MAX_CP + 1));
    for (k = 0; k < a->n; k++) {
        a->u[k] = (unsigned short)theft_random_bits(t, 16);
    }
    *out = a;
    return THEFT_ALLOC_OK;
}

static theft_hash arb_hash(const void *instance, void *env)
{
    const struct arb_units *a = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)a->u,
                              (size_t)a->n * sizeof(a->u[0]));
}

static struct theft_type_info arb_info = {
    .alloc = arb_alloc,
    .free  = theft_generic_free_cb,
    .hash  = arb_hash,
};

/* A well-formed unit sequence plus a small random output cap, for truncation. */
struct wf_cap {
    struct wf_units w;
    int             cap;     /* 0..MAX_B (encode) or 0..MAX_U (decode) */
};

static enum theft_alloc_res
wfcap_alloc(struct theft *t, void *env, void **out)
{
    struct wf_cap *c;
    void *inner;
    enum theft_alloc_res r;
    r = wf_alloc(t, env, &inner);
    if (r != THEFT_ALLOC_OK) {
        return r;
    }
    c = malloc(sizeof(*c));
    if (c == NULL) {
        free(inner);
        return THEFT_ALLOC_ERROR;
    }
    c->w = *(struct wf_units *)inner;
    free(inner);
    c->cap = (int)(theft_random_bits(t, 8) % (MAX_B + 1));   /* 0..MAX_B */
    *out = c;
    return THEFT_ALLOC_OK;
}

static theft_hash wfcap_hash(const void *instance, void *env)
{
    const struct wf_cap *c = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)c, sizeof(*c));
}

static struct theft_type_info wfcap_info = {
    .alloc = wfcap_alloc,
    .free  = theft_generic_free_cb,
    .hash  = wfcap_hash,
};

/* ============================================================= */
/* Properties                                                    */
/* ============================================================= */

static enum theft_trial_res
prop_round_trip(struct theft *t, void *arg1)
{
    struct wf_units *w = (struct wf_units *)arg1;
    unsigned char out8[MAX_B];
    unsigned short back[MAX_U];
    EncStatus s1, s2;
    int nb, nu, k;
    (void)t;

    s1 = (EncStatus)999;
    nb = Utf16ToUtf8(w->u, w->n, out8, (int)sizeof(out8), &s1);
    if (s1 != ENC_OK) {
        return THEFT_TRIAL_FAIL;       /* well-formed -> nothing lossy/truncated */
    }
    s2 = (EncStatus)999;
    nu = Utf8ToUtf16(out8, nb, back, (int)(sizeof(back) / sizeof(back[0])), &s2);
    if (s2 != ENC_OK) {
        return THEFT_TRIAL_FAIL;
    }
    if (nu != w->n) {
        return THEFT_TRIAL_FAIL;
    }
    for (k = 0; k < w->n; k++) {
        if (back[k] != w->u[k]) {
            return THEFT_TRIAL_FAIL;
        }
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_decode_total(struct theft *t, void *arg1)
{
    struct raw_bytes *r = (struct raw_bytes *)arg1;
    unsigned short out[MAX_B];          /* >= max units the bytes can yield */
    EncStatus s;
    int nu, k;
    int oracleUnits, i;
    (void)t;

    s = (EncStatus)999;
    nu = Utf8ToUtf16(r->b, r->n, out, (int)(sizeof(out) / sizeof(out[0])), &s);

    /* Total: writes a bounded number of units and terminates. */
    if (nu < 0 || nu > (int)(sizeof(out) / sizeof(out[0]))) {
        return THEFT_TRIAL_FAIL;
    }

    /* Independent byte-cursor oracle: walk the SAME Table 3-7 logic and count
     * the units a faithful decoder must produce, asserting the codec consumed
     * the WHOLE input making progress at every step (ample out buffer here, so
     * no truncation). */
    oracleUnits = 0;
    i = 0;
    while (i < r->n) {
        int adv = 0;
        uint32_t cp = 0;
        int wf = oracle_step(r->b, r->n, i, &adv, &cp);
        if (adv <= 0) {
            return THEFT_TRIAL_FAIL;    /* oracle must always advance */
        }
        if (!wf) {
            oracleUnits += 1;           /* one U+FFFD */
        } else if (cp <= 0xFFFF) {
            oracleUnits += 1;
        } else {
            oracleUnits += 2;           /* surrogate pair */
        }
        i += adv;
    }
    if (nu != oracleUnits) {
        return THEFT_TRIAL_FAIL;
    }

    /* No lone surrogate is ever emitted: every surrogate unit is a high
     * surrogate immediately followed by a low surrogate. */
    for (k = 0; k < nu; k++) {
        if (out[k] >= 0xD800 && out[k] <= 0xDBFF) {
            if (k + 1 >= nu || !(out[k + 1] >= 0xDC00 && out[k + 1] <= 0xDFFF)) {
                return THEFT_TRIAL_FAIL;
            }
            k++;                        /* consume the low half */
        } else if (out[k] >= 0xDC00 && out[k] <= 0xDFFF) {
            return THEFT_TRIAL_FAIL;    /* lone low surrogate */
        }
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_never_invalid(struct theft *t, void *arg1)
{
    struct arb_units *a = (struct arb_units *)arg1;
    unsigned char out[MAX_B];
    EncStatus s;
    int nb;
    (void)t;

    s = (EncStatus)999;
    nb = Utf16ToUtf8(a->u, a->n, out, (int)sizeof(out), &s);
    if (nb < 0 || nb > (int)sizeof(out)) {
        return THEFT_TRIAL_FAIL;
    }
    /* The load-bearing check: every byte written is well-formed UTF-8 (an
     * independent validator finds zero ill-formed sequences). */
    if (!utf8_well_formed(out, nb)) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res
prop_truncation_clean(struct theft *t, void *arg1)
{
    struct wf_cap *c = (struct wf_cap *)arg1;
    unsigned char enc[MAX_B];
    unsigned char capped[MAX_B];
    unsigned short dec[MAX_U];
    EncStatus s;
    int nb, ncapped, cap;
    (void)t;

    /* ---- encode direction: cap the UTF-8 output ---- */
    cap = c->cap;
    if (cap > (int)sizeof(capped)) {
        cap = (int)sizeof(capped);
    }
    s = (EncStatus)999;
    ncapped = Utf16ToUtf8(c->w.u, c->w.n, capped, cap, &s);
    if (ncapped < 0 || ncapped > cap) {
        return THEFT_TRIAL_FAIL;
    }
    if (s != ENC_OK && s != ENC_TRUNCATED) {
        return THEFT_TRIAL_FAIL;        /* well-formed input is never lossy */
    }
    /* The capped prefix must itself be whole, valid UTF-8 (no partial seq). */
    if (!utf8_well_formed(capped, ncapped)) {
        return THEFT_TRIAL_FAIL;
    }

    /* ---- decode direction: cap the UTF-16 output ---- */
    {
        int ucap, nu, k;
        EncStatus sd;
        /* Full encode (ample buffer) to get bytes to decode. */
        nb = Utf16ToUtf8(c->w.u, c->w.n, enc, (int)sizeof(enc), &s);
        ucap = c->cap % (MAX_U + 1);    /* 0..MAX_U */
        sd = (EncStatus)999;
        nu = Utf8ToUtf16(enc, nb, dec, ucap, &sd);
        if (nu < 0 || nu > ucap) {
            return THEFT_TRIAL_FAIL;
        }
        if (sd != ENC_OK && sd != ENC_TRUNCATED) {
            return THEFT_TRIAL_FAIL;
        }
        /* No trailing lone high surrogate: the pair is never split. */
        if (nu > 0 && dec[nu - 1] >= 0xD800 && dec[nu - 1] <= 0xDBFF) {
            return THEFT_TRIAL_FAIL;
        }
        /* And no lone low surrogate anywhere (would mean a split pair). */
        for (k = 0; k < nu; k++) {
            if (dec[k] >= 0xD800 && dec[k] <= 0xDBFF) {
                k++;                    /* skip its low half */
            } else if (is_surrogate(dec[k])) {
                return THEFT_TRIAL_FAIL;
            }
        }
    }
    return THEFT_TRIAL_PASS;
}

static int run1(const char *name, theft_propfun1 *p,
                const struct theft_type_info *ti, theft_seed seed)
{
    struct theft_run_config cfg = {
        .name = name, .prop1 = p,
        .type_info = { ti }, .trials = TRIALS, .seed = seed,
    };
    enum theft_run_res res = theft_run(&cfg);
    printf("  %-26s %s (%d trials)\n", name,
           res == THEFT_RUN_PASS ? "PASS" : "FAIL", TRIALS);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int fails = 0;

    printf("theft_encoding (src/encoding.c pure codec):\n");

    fails += run1("codec/round_trip",      prop_round_trip,
                  &wf_info,    SEED);
    fails += run1("codec/decode_total",    prop_decode_total,
                  &raw_info,   SEED ^ 0x11);
    fails += run1("codec/never_invalid",   prop_never_invalid,
                  &arb_info,   SEED ^ 0x22);
    fails += run1("codec/truncation_clean", prop_truncation_clean,
                  &wfcap_info, SEED ^ 0x33);

    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
