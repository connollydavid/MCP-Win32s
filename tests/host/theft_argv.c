/*
 * theft_argv.c - host-native property-based test for src/argv.c.
 *
 * theft host PBT harness (plan/PHASE4.md, "theft host-side PBT
 * harness"; tests/OBLIGATIONS-PHASE4.md: argv roundtrip / Q8 quoting).
 *
 * Property (autoshrinking, >= 50000 trials):
 *   ArgvJoin's output, when re-tokenized by a reference CommandLineToArgvW
 *   implementation built into this harness (Q8 rules: a run of 2N backslashes
 *   before a quote -> N backslashes + quote toggles; 2N+1 -> N backslashes +
 *   literal quote; backslashes not before a quote are literal), yields the
 *   original argv vector byte-for-byte.
 *
 * Random argv: each element is printable ASCII + space + tab + backslash +
 * quote, length 0..32, count 1..8.
 *
 * The reference tokenizer follows the post-argv[0] rules (Daniel Colascione,
 * "Everyone quotes command line arguments the wrong way"). ArgvJoin escapes
 * every element with those rules, so every element here is tokenized that way.
 *
 * Module under test stays C89; this harness is C99 + POSIX, native gcc.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdlib.h>
#include <string.h>
#include "theft.h"
#include "argv.h"

#define TRIALS    50000
#define MAX_ARGC  8
#define MAX_ARGLEN 32
#define LINE_CAP  (MAX_ARGC * (MAX_ARGLEN * 2 + 4) + 16)
#define SEED      0xA46714C0DECAFEULL

struct argvec {
    int  argc;
    char arg[MAX_ARGC][MAX_ARGLEN + 1];
};

/* Generator alphabet: bytes likely to exercise the quoting rules. */
static char pick_char(struct theft *t)
{
    /* 0..14 selects from a small interesting set; otherwise printable. */
    unsigned sel = (unsigned)theft_random_bits(t, 4);
    switch (sel) {
    case 0:  return ' ';
    case 1:  return '\t';
    case 2:  return '\\';
    case 3:  return '"';
    case 4:  return 'a';
    case 5:  return 'B';
    case 6:  return '7';
    default:
        /* printable 0x21..0x7e excluding nothing in particular */
        return (char)(0x21 + (theft_random_bits(t, 7) % (0x7e - 0x21 + 1)));
    }
}

static enum theft_alloc_res
argv_alloc(struct theft *t, void *env, void **out)
{
    struct argvec *v;
    int i, j, len;
    (void)env;

    v = malloc(sizeof(*v));
    if (v == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    /* count 1..MAX_ARGC */
    v->argc = 1 + (int)(theft_random_bits(t, 3) % MAX_ARGC);
    for (i = 0; i < v->argc; i++) {
        len = (int)(theft_random_bits(t, 6) % (MAX_ARGLEN + 1));  /* 0..32 */
        for (j = 0; j < len; j++) {
            v->arg[i][j] = pick_char(t);
        }
        v->arg[i][len] = '\0';
    }
    *out = v;
    return THEFT_ALLOC_OK;
}

static void argv_print(FILE *f, const void *instance, void *env)
{
    const struct argvec *v = (const struct argvec *)instance;
    int i;
    (void)env;
    fprintf(f, "argc=%d", v->argc);
    for (i = 0; i < v->argc; i++) {
        fprintf(f, " [%s]", v->arg[i]);
    }
    fprintf(f, "\n");
}

static struct theft_type_info argv_info = {
    .alloc = argv_alloc,
    .free  = theft_generic_free_cb,
    .print = argv_print,
    .autoshrink_config = { .enable = true },
};

/*
 * ref_tokenize - reference CommandLineToArgvW tokenizer (post-argv[0] rules,
 * ANSI). Writes out_argc tokens into out (each <= MAX bytes). Returns 0 on
 * success, -1 if it would overflow the fixed output capacity.
 *
 * Rules (Colascione / Q8):
 *  - Whitespace (space, tab) outside quotes separates tokens.
 *  - A run of N backslashes:
 *      * followed by '"': emit N/2 backslashes; if N is odd the '"' is a
 *        literal quote, else the '"' toggles the in-quotes state.
 *      * not followed by '"': emit N literal backslashes.
 *  - Inside quotes, whitespace is literal.
 */
static int ref_tokenize(const char *line, int *out_argc,
                        char out[][MAX_ARGLEN + 1], int max_tokens)
{
    const char *p = line;
    int argc = 0;
    int in_quotes = 0;
    int have_token = 0;
    int tlen = 0;
    char tok[MAX_ARGLEN * 2 + 16];

#define EMIT_TOKEN()                                              \
    do {                                                          \
        if (argc >= max_tokens) return -1;                        \
        if (tlen > MAX_ARGLEN) return -1;                         \
        memcpy(out[argc], tok, (size_t)tlen);                     \
        out[argc][tlen] = '\0';                                   \
        argc++;                                                   \
        tlen = 0; have_token = 0;                                 \
    } while (0)

#define PUSH(c)                                                   \
    do {                                                          \
        if (tlen >= (int)sizeof(tok)) return -1;                  \
        tok[tlen++] = (c); have_token = 1;                        \
    } while (0)

    for (;;) {
        /* Skip token-separating whitespace when not in a token. */
        if (!in_quotes && !have_token) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
        if (*p == '\0') {
            break;
        }

        if (!in_quotes && (*p == ' ' || *p == '\t')) {
            EMIT_TOKEN();
            continue;
        }

        if (*p == '\\') {
            int n = 0;
            while (*p == '\\') { n++; p++; }
            if (*p == '"') {
                int i;
                for (i = 0; i < n / 2; i++) { PUSH('\\'); }
                if (n % 2 == 1) {
                    PUSH('"');          /* escaped literal quote */
                    p++;
                } else {
                    in_quotes = !in_quotes;
                    have_token = 1;     /* "" still starts a token */
                    p++;
                }
            } else {
                int i;
                for (i = 0; i < n; i++) { PUSH('\\'); }
            }
            continue;
        }

        if (*p == '"') {
            in_quotes = !in_quotes;
            have_token = 1;             /* empty quoted region is a token */
            p++;
            continue;
        }

        PUSH(*p);
        p++;
    }

    if (have_token || in_quotes) {
        EMIT_TOKEN();
    }

    *out_argc = argc;
    return 0;
#undef EMIT_TOKEN
#undef PUSH
}

static enum theft_trial_res
prop_argv_roundtrip(struct theft *t, void *arg1)
{
    struct argvec *v = (struct argvec *)arg1;
    const char *cargv[MAX_ARGC];
    char line[LINE_CAP];
    char back[MAX_ARGC][MAX_ARGLEN + 1];
    int  n, back_argc, i;
    (void)t;

    for (i = 0; i < v->argc; i++) {
        cargv[i] = v->arg[i];
    }

    n = ArgvJoin(cargv, v->argc, line, (int)sizeof(line));
    if (n < 0) {
        return THEFT_TRIAL_FAIL;   /* LINE_CAP is sized to never overflow */
    }

    if (ref_tokenize(line, &back_argc, back, MAX_ARGC) != 0) {
        return THEFT_TRIAL_FAIL;
    }
    if (back_argc != v->argc) {
        return THEFT_TRIAL_FAIL;
    }
    for (i = 0; i < v->argc; i++) {
        if (strcmp(back[i], v->arg[i]) != 0) {
            return THEFT_TRIAL_FAIL;
        }
    }
    return THEFT_TRIAL_PASS;
}

int main(void)
{
    struct theft_run_config cfg = {
        .name      = "argv/join_tokenize_roundtrip",
        .prop1     = prop_argv_roundtrip,
        .type_info = { &argv_info },
        .trials    = TRIALS,
        .seed      = SEED,
    };
    enum theft_run_res res;

    printf("theft_argv (src/argv.c):\n");
    res = theft_run(&cfg);
    printf("  %-28s %s (%d trials)\n", "argv/join_tokenize_roundtrip",
           res == THEFT_RUN_PASS ? "PASS" : "FAIL", TRIALS);
    printf("%s\n", res == THEFT_RUN_PASS ? "ALL PASS" : "FAILURES");
    return res == THEFT_RUN_PASS ? 0 : 1;
}
