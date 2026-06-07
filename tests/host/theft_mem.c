/*
 * theft_mem.c - host-native property-based tests for the pure memory guards.
 *
 * 5.3 theft host PBT harness (tests/OBLIGATIONS-5.3.md, specs/memory-ops.allium:
 * SAFETY PIN #1 invariant.AddressIsWellFormed, SAFETY PIN #2
 * invariant.MemoryAccessRangeBounded - the strongest pin on the off-by-overflow
 * class). Properties (autoshrinking):
 *
 *   P1 parse_no_over_u32   MemParseU32 never accepts a value > 0xFFFFFFFF, and a
 *                          generated 32-bit value round-trips through both its
 *                          decimal and hex string forms.
 *   P2 range_no_overflow   MemRangeInBounds admits a range ONLY when its TRUE end
 *                          (computed in 64-bit here) is <= 0xFFFFFFFF AND length
 *                          <= cap - the overflow guard, cross-checked against an
 *                          oracle that cannot itself overflow.
 *
 * Only the PURE functions are exercised (no spawn/RPM/region decision). They
 * reference no Win32 type, so src/mem_ops.c is compiled with -DMEM_OPS_HOST_PURE
 * (which #ifdefs out the live Win32 paths and the <windows.h> include) and
 * linked here. This harness is C99 + POSIX, built natively with gcc.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "theft.h"

/* The two pure guards under test (declared here; mem_ops.c provides them under
 * -DMEM_OPS_HOST_PURE, free of any Win32 dependency). */
int MemParseU32(const char *s, unsigned long *out);
int MemRangeInBounds(unsigned long addr, unsigned long len, unsigned long cap);

#define TRIALS  50000
#define SEED    0x5E33ABCD0001ULL

/* ---------- P1: MemParseU32 never over-accepts; u32 round-trips ---------- */

struct u32val { uint32_t v; };

static enum theft_alloc_res
u32_alloc(struct theft *t, void *env, void **out)
{
    struct u32val *u;
    (void)env;
    u = malloc(sizeof(*u));
    if (u == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    u->v = (uint32_t)theft_random_bits(t, 32);
    *out = u;
    return THEFT_ALLOC_OK;
}

static theft_hash u32_hash(const void *instance, void *env)
{
    const struct u32val *u = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)&u->v, sizeof(u->v));
}

static struct theft_type_info u32_info = {
    .alloc = u32_alloc,
    .free  = theft_generic_free_cb,
    .hash  = u32_hash,
};

static enum theft_trial_res
prop_parse_roundtrip(struct theft *t, void *arg1)
{
    struct u32val *u = (struct u32val *)arg1;
    char dec[16];
    char hex[16];
    unsigned long out;
    (void)t;

    /* A generated 32-bit value must parse from both its decimal and hex forms,
     * recovering exactly that value (never above 0xFFFFFFFF). */
    sprintf(dec, "%lu", (unsigned long)u->v);
    sprintf(hex, "0x%lx", (unsigned long)u->v);

    out = 0;
    if (!MemParseU32(dec, &out)) {
        return THEFT_TRIAL_FAIL;
    }
    if (out != (unsigned long)u->v || out > 0xFFFFFFFFUL) {
        return THEFT_TRIAL_FAIL;
    }

    out = 0;
    if (!MemParseU32(hex, &out)) {
        return THEFT_TRIAL_FAIL;
    }
    if (out != (unsigned long)u->v || out > 0xFFFFFFFFUL) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ---------- P1b: an out-of-range hex string is never accepted ---------- */

/* A random decimal/hex string with up to 12 digits: most overflow 32 bits, so
 * this hammers the over-accept side - any accepted value must be <= 0xFFFFFFFF
 * and equal to a 64-bit re-parse of the same digits. */
struct numstr { char s[20]; };

static enum theft_alloc_res
numstr_alloc(struct theft *t, void *env, void **out)
{
    struct numstr *n;
    int len, i, hex;
    (void)env;
    n = malloc(sizeof(*n));
    if (n == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    hex = (int)(theft_random_bits(t, 1));
    len = 1 + (int)(theft_random_bits(t, 4) % 12);   /* 1..12 digits */
    i = 0;
    if (hex) {
        n->s[i++] = '0';
        n->s[i++] = 'x';
    }
    {
        int d;
        for (d = 0; d < len; d++) {
            if (hex) {
                int nib = (int)(theft_random_bits(t, 4) % 16);
                n->s[i++] = (char)(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
            } else {
                n->s[i++] = (char)('0' + (theft_random_bits(t, 4) % 10));
            }
        }
    }
    n->s[i] = '\0';
    *out = n;
    return THEFT_ALLOC_OK;
}

static struct theft_type_info numstr_info = {
    .alloc = numstr_alloc,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
};

static enum theft_trial_res
prop_parse_never_over(struct theft *t, void *arg1)
{
    struct numstr *n = (struct numstr *)arg1;
    unsigned long out;
    unsigned long long oracle;
    const char *p;
    int hex;
    (void)t;

    /* 64-bit oracle parse of the same digits (cannot itself overflow at <=12
     * digits). */
    p = n->s;
    hex = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        hex = 1;
        p += 2;
    }
    oracle = 0;
    for (; *p != '\0'; p++) {
        char c = *p;
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            return THEFT_TRIAL_SKIP;   /* should not happen by construction */
        }
        oracle = oracle * (hex ? 16ULL : 10ULL) + d;
    }

    out = 0xDEADBEEFUL;
    if (MemParseU32(n->s, &out)) {
        /* Accepted: must be in range and equal the oracle. */
        if (oracle > 0xFFFFFFFFULL) {
            return THEFT_TRIAL_FAIL;   /* over-accept of a >32-bit value */
        }
        if ((unsigned long long)out != oracle) {
            return THEFT_TRIAL_FAIL;
        }
    } else {
        /* Rejected: must be because the value truly overflows 32 bits. */
        if (oracle <= 0xFFFFFFFFULL) {
            return THEFT_TRIAL_FAIL;   /* false reject of an in-range value */
        }
    }
    return THEFT_TRIAL_PASS;
}

/* ---------- P2: MemRangeInBounds is overflow-safe ---------- */

struct rangecase { uint32_t addr; uint32_t len; uint32_t cap; };

static enum theft_alloc_res
range_alloc(struct theft *t, void *env, void **out)
{
    struct rangecase *r;
    (void)env;
    r = malloc(sizeof(*r));
    if (r == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    /* Bias addr toward the high end so addr+len wraparound is hit often. */
    if (theft_random_bits(t, 1)) {
        r->addr = (uint32_t)(0xFFFFFFFFUL -
                             (theft_random_bits(t, 17) & 0x1FFFF));
    } else {
        r->addr = (uint32_t)theft_random_bits(t, 32);
    }
    r->len = (uint32_t)theft_random_bits(t, 18);   /* 0 .. 0x3FFFF */
    r->cap = (uint32_t)theft_random_bits(t, 18);
    *out = r;
    return THEFT_ALLOC_OK;
}

static theft_hash range_hash(const void *instance, void *env)
{
    const struct rangecase *r = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)r, sizeof(*r));
}

static struct theft_type_info range_info = {
    .alloc = range_alloc,
    .free  = theft_generic_free_cb,
    .hash  = range_hash,
};

static enum theft_trial_res
prop_range_no_overflow(struct theft *t, void *arg1)
{
    struct rangecase *r = (struct rangecase *)arg1;
    int got;
    int oracle;
    unsigned long long end;
    (void)t;

    got = MemRangeInBounds((unsigned long)r->addr,
                           (unsigned long)r->len,
                           (unsigned long)r->cap);

    /* 64-bit oracle: admit iff len <= cap AND addr+len <= 0xFFFFFFFF, computed
     * where no wraparound can hide a violation. */
    end = (unsigned long long)r->addr + (unsigned long long)r->len;
    oracle = (r->len <= r->cap && end <= 0xFFFFFFFFULL) ? 1 : 0;

    if (got != oracle) {
        return THEFT_TRIAL_FAIL;
    }
    /* The load-bearing direction: a true end past 0xFFFFFFFF is NEVER admitted. */
    if (got && end > 0xFFFFFFFFULL) {
        return THEFT_TRIAL_FAIL;
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
    printf("  %-28s %s (%d trials)\n", name,
           res == THEFT_RUN_PASS ? "PASS" : "FAIL", TRIALS);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int fails = 0;

    printf("theft_mem (src/mem_ops.c pure guards):\n");

    fails += run1("mem/parse_roundtrip",   prop_parse_roundtrip,
                  &u32_info,    SEED);
    fails += run1("mem/parse_never_over",  prop_parse_never_over,
                  &numstr_info, SEED ^ 0x11);
    fails += run1("mem/range_no_overflow", prop_range_no_overflow,
                  &range_info,  SEED ^ 0x22);

    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
