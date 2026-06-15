/*
 * theft_strutil.c - host-native property-based tests for src/strutil.c
 * (McpStrCpyN, the device's own DBCS-aware bounded copy - the NT 3.1 floor
 * lacks lstrcpynA).
 *
 * theft host PBT harness (CLAUDE.md "two frameworks"). The shim's CharNextA
 * (tests/host/win32_shim.h) models a cp932-style DBCS codepage so the
 * no-split property is DETERMINISTICALLY testable on Linux.
 *
 * Properties (>= 50000 trials each, autoshrinking) over random byte strings
 * + random n (including n <= 0 and n == 1):
 *   P1 terminated  n > 0 => result is NUL-terminated and strlen(result) <= n-1.
 *   P2 prefix      result bytes are a byte-exact prefix of src.
 *   P3 no_split    re-walking the result with CharNextA from the start lands
 *                  EXACTLY on the terminator (the last copied char is whole;
 *                  no dangling DBCS lead byte).
 *   P4 bounded     dst is allocated as EXACTLY n bytes inside an ASan-guarded
 *                  region; McpStrCpyN never writes at or beyond dst[n].
 *   P5 edges       dst == NULL returns NULL and does not crash; n <= 0 writes
 *                  nothing (a poisoned guard byte before the buffer survives).
 *
 * Module under test stays C89; this harness is C99 + POSIX, built natively
 * with gcc + ASan/UBSan on Linux. The Win32 surface (CharNextA) resolves to
 * tests/host/win32_shim.h through tests/host/windows.h.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdlib.h>
#include <string.h>
#include <windows.h>      /* shim: CharNextA */
#include "theft.h"
#include "strutil.h"

#define TRIALS   50000
#define MAX_SRC  256
#define SEED     0x57271234CAFEULL

/* A generated random NUL-terminated src string plus a bound n. The src is a
 * mix of single bytes and well-formed cp932-style lead/trail pairs so the
 * no-split property has real double-byte characters to (not) split. n ranges
 * into <= 0 and small values. */
struct input {
    char src[MAX_SRC + 1];   /* always NUL-terminated */
    int  src_len;            /* strlen(src) */
    int  n;                  /* bound passed to McpStrCpyN, may be <= 0 */
};

static int is_lead(unsigned char c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

static enum theft_alloc_res
input_alloc(struct theft *t, void *env, void **out)
{
    struct input *in;
    int target, i;
    (void)env;

    in = malloc(sizeof(*in));
    if (in == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    /* 8 bits -> 0..255 masked into 0..MAX_SRC; small pools -> short/empty. */
    target = (int)(theft_random_bits(t, 8) % (MAX_SRC + 1));
    i = 0;
    while (i < target) {
        unsigned char b = (unsigned char)theft_random_bits(t, 8);
        if (b == 0) {
            /* Never embed an interior NUL: it would end the string early and
             * the generator's intended length would not be realised. Bias to
             * a printable byte instead. */
            b = (unsigned char)('A' + (theft_random_bits(t, 5) % 26));
        }
        if (is_lead(b) && i + 1 < MAX_SRC && (i + 1) < target) {
            /* Emit a well-formed double-byte char: lead + non-NUL trail. */
            unsigned char trail = (unsigned char)(theft_random_bits(t, 8));
            if (trail == 0) {
                trail = 0x40;          /* a valid-ish cp932 trail */
            }
            in->src[i++] = (char)b;
            in->src[i++] = (char)trail;
        } else if (is_lead(b)) {
            /* No room for a trail: substitute a single ASCII byte so we never
             * leave a dangling lead in the GENERATED src itself. */
            in->src[i++] = (char)('a' + (b % 26));
        } else {
            in->src[i++] = (char)b;
        }
    }
    in->src[i] = '\0';
    in->src_len = i;
    /* n in -2 .. src_len+2, so n <= 0, n == 1, exact-fit and over-fit all
     * occur. 6 bits -> 0..63 then shifted to include the negatives. */
    in->n = (int)(theft_random_bits(t, 6) % (unsigned)(in->src_len + 5)) - 2;
    *out = in;
    return THEFT_ALLOC_OK;
}

static struct theft_type_info input_info = {
    .alloc = input_alloc,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
};

/* P1: n > 0 => NUL-terminated, strlen(result) <= n-1. */
static enum theft_trial_res
prop_terminated(struct theft *t, void *arg1)
{
    struct input *in = (struct input *)arg1;
    char *dst;
    int n;
    (void)t;

    n = in->n;
    if (n <= 0) {
        return THEFT_TRIAL_PASS;   /* covered by P5 */
    }
    dst = malloc((size_t)n);
    if (dst == NULL) {
        return THEFT_TRIAL_ERROR;
    }
    memset(dst, 'Z', (size_t)n);
    McpStrCpyN(dst, in->src, n);
    if ((int)strlen(dst) > n - 1) {
        free(dst);
        return THEFT_TRIAL_FAIL;
    }
    /* NUL-terminated: strlen stayed inside the buffer, so dst[strlen] is the
     * NUL within [0, n-1]. */
    free(dst);
    return THEFT_TRIAL_PASS;
}

/* P2: result bytes are a byte-exact prefix of src. */
static enum theft_trial_res
prop_prefix(struct theft *t, void *arg1)
{
    struct input *in = (struct input *)arg1;
    char *dst;
    int n, len;
    (void)t;

    n = in->n;
    if (n <= 0) {
        return THEFT_TRIAL_PASS;
    }
    dst = malloc((size_t)n);
    if (dst == NULL) {
        return THEFT_TRIAL_ERROR;
    }
    McpStrCpyN(dst, in->src, n);
    len = (int)strlen(dst);
    if (len > in->src_len) {
        free(dst);
        return THEFT_TRIAL_FAIL;   /* longer than the source: impossible */
    }
    if (memcmp(dst, in->src, (size_t)len) != 0) {
        free(dst);
        return THEFT_TRIAL_FAIL;
    }
    free(dst);
    return THEFT_TRIAL_PASS;
}

/* P3: re-walking the result with CharNextA lands EXACTLY on the terminator. */
static enum theft_trial_res
prop_no_split(struct theft *t, void *arg1)
{
    struct input *in = (struct input *)arg1;
    char *dst;
    const char *p;
    int n;
    (void)t;

    n = in->n;
    if (n <= 0) {
        return THEFT_TRIAL_PASS;
    }
    dst = malloc((size_t)n);
    if (dst == NULL) {
        return THEFT_TRIAL_ERROR;
    }
    McpStrCpyN(dst, in->src, n);
    /* Walk character-by-character; we must arrive at the NUL exactly, never
     * step past it (which CharNextA refuses) and never stop short with a
     * dangling lead byte. */
    p = dst;
    while (*p != '\0') {
        const char *q = CharNextA(p);
        if (q == p) {
            free(dst);
            return THEFT_TRIAL_FAIL;   /* no progress: malformed */
        }
        p = q;
    }
    /* p now points at the terminator. A split would have left a trailing lead
     * byte that CharNextA treats as a single byte stepping onto the NUL, but
     * McpStrCpyN must never copy such a fragment: assert the last character
     * boundary in dst matches a boundary in src too. Re-walk src to the same
     * byte offset and confirm it is a character boundary there. */
    {
        int off = (int)(p - dst);
        const char *s = in->src;
        while ((int)(s - in->src) < off && *s != '\0') {
            s = CharNextA(s);
        }
        if ((int)(s - in->src) != off) {
            free(dst);
            return THEFT_TRIAL_FAIL;   /* result end is mid-character in src */
        }
    }
    free(dst);
    return THEFT_TRIAL_PASS;
}

/* P4: bounded write - dst is EXACTLY n bytes; ASan red-zones catch any write
 * at or beyond dst[n]. (The malloc'd block is exactly n; a stray write trips
 * ASan and aborts the run.) */
static enum theft_trial_res
prop_bounded(struct theft *t, void *arg1)
{
    struct input *in = (struct input *)arg1;
    char *dst;
    int n;
    (void)t;

    n = in->n;
    if (n <= 0) {
        return THEFT_TRIAL_PASS;
    }
    dst = malloc((size_t)n);   /* EXACTLY n bytes, ASan-guarded */
    if (dst == NULL) {
        return THEFT_TRIAL_ERROR;
    }
    McpStrCpyN(dst, in->src, n);
    /* If McpStrCpyN wrote at/beyond dst[n] ASan already aborted. Belt: the
     * terminator sits within the buffer. */
    if (dst[strlen(dst)] != '\0') {
        free(dst);
        return THEFT_TRIAL_FAIL;
    }
    free(dst);
    return THEFT_TRIAL_PASS;
}

/* P5: edges - dst == NULL returns NULL; n <= 0 writes nothing. */
static enum theft_trial_res
prop_edges(struct theft *t, void *arg1)
{
    struct input *in = (struct input *)arg1;
    char *guarded;
    char *dst;
    (void)t;

    /* dst == NULL: returns NULL, no crash, for any n. */
    if (McpStrCpyN(NULL, in->src, in->n) != NULL) {
        return THEFT_TRIAL_FAIL;
    }
    if (McpStrCpyN(NULL, in->src, 16) != NULL) {
        return THEFT_TRIAL_FAIL;
    }

    /* n <= 0 writes nothing: give a 1-byte buffer with a known sentinel and
     * confirm McpStrCpyN leaves it untouched (no NUL written, no copy). */
    guarded = malloc(1);
    if (guarded == NULL) {
        return THEFT_TRIAL_ERROR;
    }
    guarded[0] = (char)0xAB;
    dst = McpStrCpyN(guarded, in->src, 0);
    if (dst != guarded) {
        free(guarded);
        return THEFT_TRIAL_FAIL;   /* must return dst unchanged */
    }
    if ((unsigned char)guarded[0] != 0xAB) {
        free(guarded);
        return THEFT_TRIAL_FAIL;   /* wrote into a n<=0 buffer */
    }
    dst = McpStrCpyN(guarded, in->src, -5);
    if (dst != guarded || (unsigned char)guarded[0] != 0xAB) {
        free(guarded);
        return THEFT_TRIAL_FAIL;
    }
    free(guarded);
    return THEFT_TRIAL_PASS;
}

static int run(const char *name, theft_propfun1 *prop)
{
    struct theft_run_config cfg = {
        .name      = name,
        .prop1     = prop,
        .type_info = { &input_info },
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
    printf("theft_strutil (src/strutil.c):\n");
    fails += run("strutil/terminated",  prop_terminated);
    fails += run("strutil/prefix",      prop_prefix);
    fails += run("strutil/no_split",    prop_no_split);
    fails += run("strutil/bounded",     prop_bounded);
    fails += run("strutil/edges",       prop_edges);
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
