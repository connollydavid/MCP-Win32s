/*
 * theft_base64.c - host-native property-based tests for src/base64.c.
 *
 * theft host PBT harness (plan/PHASE4.md, "theft host-side PBT
 * harness"). Deep, autoshrinking version of tests/test_pbt_base64.c.
 *
 * Properties (>= 50000 trials each, autoshrinking):
 *   P1 roundtrip      decode(encode(x)) == x for any byte buffer 0..8192.
 *   P2 alphabet       every byte of encode(x) is in [A-Za-z0-9+/=].
 *   P3 length formula len(encode(x)) == 4*ceil(n/3) for n > 0 (0 -> 0).
 *
 * Module under test stays C89; this harness is C99 + POSIX, built natively
 * with gcc on Linux (the one documented exception, CLAUDE.md "two frameworks").
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdlib.h>
#include <string.h>
#include "theft.h"
#include "base64.h"

#define TRIALS  50000
#define MAX_BUF 8192
#define SEED    0x5eed1234CAFEULL

/* A generated random byte buffer. */
struct buf {
    int            len;
    unsigned char  bytes[MAX_BUF];
};

/* alloc: smaller bit values must yield simpler instances (autoshrink). */
static enum theft_alloc_res
buf_alloc(struct theft *t, void *env, void **out)
{
    struct buf *b;
    int i;
    (void)env;

    b = malloc(sizeof(*b));
    if (b == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    /* 14 bits -> 0..16383, masked into 0..MAX_BUF; small pools -> len 0. */
    b->len = (int)(theft_random_bits(t, 14) % (MAX_BUF + 1));
    for (i = 0; i < b->len; i++) {
        b->bytes[i] = (unsigned char)theft_random_bits(t, 8);
    }
    *out = b;
    return THEFT_ALLOC_OK;
}

static struct theft_type_info buf_info = {
    .alloc = buf_alloc,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
};

static int is_b64_char(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '+' || c == '/' || c == '=';
}

/* P1: decode(encode(x)) == x */
static enum theft_trial_res
prop_roundtrip(struct theft *t, void *arg1)
{
    struct buf *b = (struct buf *)arg1;
    char           enc[MAX_BUF * 2 + 8];
    unsigned char  dec[MAX_BUF + 8];
    int            enc_len, dec_len;
    (void)t;

    enc_len = Base64Encode(b->bytes, b->len, enc, (int)sizeof(enc));
    /* Encode returns 0 for an empty buffer (documented), and a positive
     * length otherwise; the buffer is always big enough here. */
    if (b->len > 0 && enc_len <= 0) {
        return THEFT_TRIAL_FAIL;
    }
    dec_len = Base64Decode(enc, dec, (int)sizeof(dec));
    if (dec_len != b->len) {
        return THEFT_TRIAL_FAIL;
    }
    if (b->len > 0 && memcmp(dec, b->bytes, (size_t)b->len) != 0) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* P2: encoded output uses only the base64 alphabet + padding. */
static enum theft_trial_res
prop_alphabet(struct theft *t, void *arg1)
{
    struct buf *b = (struct buf *)arg1;
    char enc[MAX_BUF * 2 + 8];
    int  enc_len, i;
    (void)t;

    enc_len = Base64Encode(b->bytes, b->len, enc, (int)sizeof(enc));
    for (i = 0; i < enc_len; i++) {
        if (!is_b64_char((unsigned char)enc[i])) {
            return THEFT_TRIAL_FAIL;
        }
    }
    if (enc[enc_len] != '\0') {
        return THEFT_TRIAL_FAIL;   /* must be NUL-terminated */
    }
    return THEFT_TRIAL_PASS;
}

/* P3: len(encode(x)) == 4*ceil(n/3) for n > 0; 0 for n == 0. */
static enum theft_trial_res
prop_length(struct theft *t, void *arg1)
{
    struct buf *b = (struct buf *)arg1;
    char enc[MAX_BUF * 2 + 8];
    int  enc_len, expected;
    (void)t;

    enc_len = Base64Encode(b->bytes, b->len, enc, (int)sizeof(enc));
    expected = (b->len == 0) ? 0 : ((b->len + 2) / 3) * 4;
    if (enc_len != expected) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static int run(const char *name, theft_propfun1 *prop)
{
    struct theft_run_config cfg = {
        .name      = name,
        .prop1     = prop,
        .type_info = { &buf_info },
        .trials    = TRIALS,
        .seed      = SEED,
    };
    enum theft_run_res res = theft_run(&cfg);
    printf("  %-28s %s (%d trials)\n", name,
           res == THEFT_RUN_PASS ? "PASS" : "FAIL", TRIALS);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int fails = 0;
    printf("theft_base64 (src/base64.c):\n");
    fails += run("base64/roundtrip",        prop_roundtrip);
    fails += run("base64/alphabet_validity", prop_alphabet);
    fails += run("base64/length_formula",   prop_length);
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
