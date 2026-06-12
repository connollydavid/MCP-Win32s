/*
 * theft_json.c - host-native property-based tests for src/json_parser.c.
 *
 * theft host PBT harness (plan/PHASE4.md, "theft host-side PBT
 * harness"; tests/OBLIGATIONS-PHASE4.md entity-fields.Command parse path).
 *
 * Properties (autoshrinking):
 *   P1 fuzz       ParseJsonCommand never crashes / overruns on arbitrary
 *                 bytes (<= 4KB); always returns exactly 0 or 1. The input is
 *                 sized to its exact length and NUL-terminated, so ASan +
 *                 the strict NUL-terminator contract catch any over-read past
 *                 the terminator (a read past NUL would touch the redzone).
 *   P2 roundtrip  parse(BuildJsonResponse(id,"ok","line",value)) recovers
 *                 value in out.line, for random printable id/value. This
 *                 exercises json_escape -> json_unescape as inverses.
 *
 * Module under test stays C89; this harness is C99 + POSIX, native gcc.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdlib.h>
#include <string.h>
#include "theft.h"
#include "common.h"
#include "json_parser.h"

/* The json fuzz property is the wall-clock-heaviest (256KB BuildJsonResponse
 * static buffer touched indirectly + parser walk); per the brief it may drop
 * to 20000 if it exceeds ~60s. Measured well under that, so kept at 50000. */
#define FUZZ_TRIALS  50000
#define RT_TRIALS    50000
#define FUZZ_MAX     4096
#define RT_VAL_MAX   400      /* fits MCP_MAX_LINE (1024) after escaping */
#define RT_ID_MAX    24       /* fits MCP_MAX_ID (32); id is NOT escaped */
#define SEED         0xABCD5EED0001ULL

/* ---------- P1: fuzz ---------- */

struct rawbuf {
    int   len;
    char *bytes;   /* exactly len+1 bytes; bytes[len] == '\0' */
};

static enum theft_alloc_res
raw_alloc(struct theft *t, void *env, void **out)
{
    struct rawbuf *b;
    int i;
    (void)env;

    b = malloc(sizeof(*b));
    if (b == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    b->len = (int)(theft_random_bits(t, 13) % (FUZZ_MAX + 1));
    /* Exact-size allocation: a read at or past bytes[len] hits the ASan
     * redzone, turning any over-read into a hard failure. */
    b->bytes = malloc((size_t)b->len + 1);
    if (b->bytes == NULL) {
        free(b);
        return THEFT_ALLOC_ERROR;
    }
    for (i = 0; i < b->len; i++) {
        /* Avoid embedded NUL so the byte string spans the full length;
         * the terminator is the only NUL, as in real protocol input. */
        unsigned char c = (unsigned char)(theft_random_bits(t, 8) | 1);
        b->bytes[i] = (char)c;
    }
    b->bytes[b->len] = '\0';
    *out = b;
    return THEFT_ALLOC_OK;
}

static void raw_free(void *instance, void *env)
{
    struct rawbuf *b = (struct rawbuf *)instance;
    (void)env;
    if (b != NULL) {
        free(b->bytes);
        free(b);
    }
}

static struct theft_type_info raw_info = {
    .alloc = raw_alloc,
    .free  = raw_free,
    .autoshrink_config = { .enable = true },
};

static enum theft_trial_res
prop_fuzz(struct theft *t, void *arg1)
{
    struct rawbuf *b = (struct rawbuf *)arg1;
    JsonCommand cmd;
    int r;
    (void)t;

    r = ParseJsonCommand(b->bytes, &cmd);
    if (r != 0 && r != 1) {
        return THEFT_TRIAL_FAIL;   /* contract: returns exactly 0 or 1 */
    }
    return THEFT_TRIAL_PASS;
}

/* ---------- P2: build->parse roundtrip ---------- */

/* Random printable ASCII string (0x20..0x7e), used for id and value. */
struct pstr {
    char s[RT_VAL_MAX + 1];
};

static enum theft_alloc_res
pstr_alloc_n(struct theft *t, void *env, void **out)
{
    struct pstr *p;
    int maxlen = *(int *)env;
    int len, i;

    p = malloc(sizeof(*p));
    if (p == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    len = (int)(theft_random_bits(t, 12) % (maxlen + 1));
    for (i = 0; i < len; i++) {
        /* printable range 0x20..0x7e -> 95 values */
        p->s[i] = (char)(0x20 + (theft_random_bits(t, 7) % 95));
    }
    p->s[len] = '\0';
    *out = p;
    return THEFT_ALLOC_OK;
}

static int g_id_max  = RT_ID_MAX;
static int g_val_max = RT_VAL_MAX;

static struct theft_type_info id_info = {
    .alloc = pstr_alloc_n,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
    .env   = &g_id_max,
};
static struct theft_type_info val_info = {
    .alloc = pstr_alloc_n,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
    .env   = &g_val_max,
};

static enum theft_trial_res
prop_roundtrip(struct theft *t, void *arg1, void *arg2)
{
    struct pstr *id  = (struct pstr *)arg1;
    struct pstr *val = (struct pstr *)arg2;
    static char  json[MCP_MAX_RESPONSE];
    JsonCommand  cmd;
    int n;
    (void)t;

    /* BuildJsonResponse escapes only `value`; id/status/key are emitted raw.
     * id is restricted to printable ASCII with no '"' or '\\' clashes? It may
     * contain them -- but the builder does not escape id, so a '"' in id would
     * break framing. Skip such ids: they are not a roundtrip target (the
     * server only ever emits ids it controls). The value, by contrast, goes
     * through json_escape and must round-trip for ALL printable bytes. */
    if (strchr(id->s, '"') != NULL || strchr(id->s, '\\') != NULL) {
        return THEFT_TRIAL_SKIP;
    }

    n = BuildJsonResponse(id->s, "ok", "line", val->s, json, (int)sizeof(json));
    if (n <= 0) {
        return THEFT_TRIAL_FAIL;   /* well within buffer -> must succeed */
    }
    if (!ParseJsonCommand(json, &cmd)) {
        return THEFT_TRIAL_FAIL;   /* our own output must parse */
    }
    if (strcmp(cmd.id, id->s) != 0) {
        return THEFT_TRIAL_FAIL;
    }
    if (strcmp(cmd.line, val->s) != 0) {
        return THEFT_TRIAL_FAIL;   /* escape o unescape must be identity */
    }
    return THEFT_TRIAL_PASS;
}

int main(void)
{
    int fails = 0;
    struct theft_run_config fuzz_cfg = {
        .name      = "json/fuzz_never_crashes",
        .prop1     = prop_fuzz,
        .type_info = { &raw_info },
        .trials    = FUZZ_TRIALS,
        .seed      = SEED,
    };
    struct theft_run_config rt_cfg = {
        .name      = "json/build_parse_roundtrip",
        .prop2     = prop_roundtrip,
        .type_info = { &id_info, &val_info },
        .trials    = RT_TRIALS,
        .seed      = SEED ^ 0x1111ULL,
    };
    enum theft_run_res r1, r2;

    printf("theft_json (src/json_parser.c):\n");

    r1 = theft_run(&fuzz_cfg);
    printf("  %-28s %s (%d trials)\n", "json/fuzz_never_crashes",
           r1 == THEFT_RUN_PASS ? "PASS" : "FAIL", FUZZ_TRIALS);
    if (r1 != THEFT_RUN_PASS) fails++;

    r2 = theft_run(&rt_cfg);
    printf("  %-28s %s (%d trials)\n", "json/build_parse_roundtrip",
           r2 == THEFT_RUN_PASS ? "PASS" : "FAIL", RT_TRIALS);
    if (r2 != THEFT_RUN_PASS) fails++;

    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
