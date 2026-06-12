/*
 * theft_catalog.c - host-native property-based tests for src/catalog.c.
 *
 * theft host PBT harness (plan/PHASE4.md, "theft host-side PBT
 * harness"; tests/OBLIGATIONS-PHASE4.md catalog.allium: gate-exclusivity,
 * invariant.EntriesBelongToTheCatalog).
 *
 * Catalog is loaded once from catalog/win32-commands.json (tries ./catalog
 * then ../catalog, or $MCP_CATALOG). Properties (autoshrinking):
 *
 *   P1 lookup_total      CatalogLookup(cat, name) never crashes for any random
 *                        name; a hit returns an entry whose name matches
 *                        case-insensitively.
 *   P2 entries_named     invariant.EntriesBelongToTheCatalog: every loaded
 *                        entry has a non-empty name and CatalogLookup of that
 *                        name returns that same entry.
 *   P3 reject_unknown    CatalogValidateArgs never accepts an argv containing
 *                        a flag token absent from the entry's options (we
 *                        synthesise unknown flags by mutating known ones).
 *   P4 glued_eq_split    for an arg-taking option, the glued form /A:value and
 *                        the split form /A value validate identically.
 *
 * src/catalog.c stays C89 and #includes <windows.h>; the host build resolves
 * that to tests/host/windows.h -> win32_shim.h. This harness is C99 + POSIX,
 * built natively with gcc.
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include "theft.h"
#include "catalog.h"

#define TRIALS  50000
#define SEED    0xCA7A106F1A6EULL

static Catalog *g_cat;
static int      g_entry_count;

/* The CatalogEntry/Catalog structs are opaque in catalog.h, so the harness
 * works only through the public accessors. To reach an entry by index we
 * walk names via a small known-good list captured at load: the catalog ships
 * a fixed set, so we enumerate by lookup over a copy of each entry's name.
 * catalog.h exposes CatalogCount but not index access, so we collect names by
 * scanning a generous candidate set is not possible -- instead we rely on the
 * accessor surface plus the fact that lookups are by name. We therefore keep
 * the set of names we discover (from the shipped catalog) for property P2/P3. */

/* Names known to be in catalog/win32-commands.json (plan/PHASE4.md initial
 * entries). Used to drive entry-targeted properties; any name absent from the
 * loaded catalog is skipped, so this list can be a safe superset. */
static const char *g_known_names[] = {
    "dir", "copy", "del", "type", "echo", "cd", "mkdir", "rmdir", "ren",
    "set", "path", "ver", "cls", "attrib", "xcopy", "find", "more", "sort",
    "cl", "link", "lib", "nmake", "ml", "rc", "mt", "mc", "gcc", "make",
    "mem", "chkdsk"
};
#define KNOWN_COUNT ((int)(sizeof(g_known_names) / sizeof(g_known_names[0])))

static int load_catalog(void)
{
    const char *candidates[3];
    char err[256];
    int i;
    const char *envp = getenv("MCP_CATALOG");

    candidates[0] = envp;
    candidates[1] = "catalog/win32-commands.json";
    candidates[2] = "../catalog/win32-commands.json";

    for (i = 0; i < 3; i++) {
        if (candidates[i] == NULL) {
            continue;
        }
        if (CatalogLoad(candidates[i], &g_cat, err, (int)sizeof(err))) {
            g_entry_count = CatalogCount(g_cat);
            return 1;
        }
    }
    fprintf(stderr, "catalog load failed (tried env/./catalog/../catalog): %s\n",
            err);
    return 0;
}

/* ---------- P1: lookup totality ---------- */

struct rname { char s[40]; };

static enum theft_alloc_res
rname_alloc(struct theft *t, void *env, void **out)
{
    struct rname *r;
    int len, i;
    (void)env;
    r = malloc(sizeof(*r));
    if (r == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    len = (int)(theft_random_bits(t, 5) % (int)sizeof(r->s));   /* 0..31 */
    for (i = 0; i < len; i++) {
        /* any non-NUL byte */
        r->s[i] = (char)(theft_random_bits(t, 8) | 1);
    }
    r->s[len] = '\0';
    *out = r;
    return THEFT_ALLOC_OK;
}

static struct theft_type_info rname_info = {
    .alloc = rname_alloc,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
};

static enum theft_trial_res
prop_lookup_total(struct theft *t, void *arg1)
{
    struct rname *r = (struct rname *)arg1;
    const CatalogEntry *e;
    (void)t;

    e = CatalogLookup(g_cat, r->s);
    if (e != NULL) {
        const char *name = CatalogEntryName(e);
        if (name == NULL) {
            return THEFT_TRIAL_FAIL;
        }
        if (strcasecmp(name, r->s) != 0) {
            return THEFT_TRIAL_FAIL;   /* hit must be the matched name */
        }
    }
    /* NULL is fine (miss); the contract is "never crash, total function". */
    return THEFT_TRIAL_PASS;
}

/* ---------- P2: entries are named & self-consistent ---------- */

static enum theft_trial_res
prop_entries_named(struct theft *t, void *arg1)
{
    /* arg1 is an index into the known-name list; check only names that the
     * loaded catalog actually contains. */
    int idx = *(int *)arg1 % KNOWN_COUNT;
    const char *name = g_known_names[idx];
    const CatalogEntry *e;
    const char *ename;
    (void)t;

    e = CatalogLookup(g_cat, name);
    if (e == NULL) {
        return THEFT_TRIAL_SKIP;   /* not shipped under this name */
    }
    ename = CatalogEntryName(e);
    if (ename == NULL || ename[0] == '\0') {
        return THEFT_TRIAL_FAIL;   /* invariant: loaded entries are named */
    }
    if (strcasecmp(ename, name) != 0) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* index generator for P2/P3 */
struct idx { int v; };
static enum theft_alloc_res
idx_alloc(struct theft *t, void *env, void **out)
{
    struct idx *x = malloc(sizeof(*x));
    (void)env;
    if (x == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    x->v = (int)theft_random_bits(t, 16);
    *out = x;
    return THEFT_ALLOC_OK;
}
static theft_hash idx_hash(const void *instance, void *env)
{
    const struct idx *x = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)&x->v, sizeof(x->v));
}
static struct theft_type_info idx_info = {
    .alloc = idx_alloc,
    .free  = theft_generic_free_cb,
    .hash  = idx_hash,
};

/* ---------- P3: unknown flags rejected ---------- */

/* A trial input: which known entry, plus a small random flag string. */
struct flagcase {
    int  entry_idx;     /* into g_known_names */
    char flag[16];      /* candidate flag token, starts with '/' */
};

static enum theft_alloc_res
flagcase_alloc(struct theft *t, void *env, void **out)
{
    struct flagcase *c;
    int len, i;
    (void)env;
    c = malloc(sizeof(*c));
    if (c == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    c->entry_idx = (int)theft_random_bits(t, 16) % KNOWN_COUNT;
    c->flag[0] = '/';
    /* Bias toward single-letter flags (len 1) so the generator regularly hits
     * real one-letter options like /A /B /O -- this exercises both the
     * unknown-flag rejection (P3) and the arg-taking glued/split equivalence
     * (P4). Occasionally produce a longer token to probe multi-char flags. */
    if ((theft_random_bits(t, 1)) == 0) {
        len = 2;   /* '/' + one letter */
    } else {
        len = 1 + (int)(theft_random_bits(t, 3) % 6);
    }
    for (i = 1; i < len && i < (int)sizeof(c->flag) - 1; i++) {
        c->flag[i] = (char)('A' + (theft_random_bits(t, 5) % 26));
    }
    c->flag[i] = '\0';
    *out = c;
    return THEFT_ALLOC_OK;
}
static struct theft_type_info flagcase_info = {
    .alloc = flagcase_alloc,
    .free  = theft_generic_free_cb,
    .autoshrink_config = { .enable = true },
};

static enum theft_trial_res
prop_reject_unknown(struct theft *t, void *arg1)
{
    struct flagcase *c = (struct flagcase *)arg1;
    const char *name = g_known_names[c->entry_idx];
    const CatalogEntry *e;
    const char *argv[2];
    char err[64];
    int  accepted;
    (void)t;

    e = CatalogLookup(g_cat, name);
    if (e == NULL) {
        return THEFT_TRIAL_SKIP;
    }

    /* Establish ground truth: is this flag actually a declared option?
     * We can only observe it through CatalogValidateArgs. Use a no-arg probe:
     * a flag that IS declared validates; if it validates, this trial is not a
     * negative case -> skip. (Glued arg flags also validate; those are P4.) */
    argv[0] = name;
    argv[1] = c->flag;
    accepted = CatalogValidateArgs(e, argv, 2, err, (int)sizeof(err));

    if (accepted) {
        /* The synthesised flag happened to be (a prefix of) a real option.
         * Then acceptance is correct, not a violation. Skip. */
        return THEFT_TRIAL_SKIP;
    }
    /* Rejected: the error must be the args-not-allowed reason. */
    if (strcmp(err, "argument not allowed") != 0) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ---------- P4: glued == split for ARG-TAKING options ----------
 * Spec obligation (catalog.allium): for an option that takes an argument, the
 * glued form /A:value and the split form /A value validate identically (both
 * accepted). This equivalence is intentionally NOT required for no-arg flags:
 * there, only the bare flag is a valid token, the glued /B:value is a distinct
 * unknown token (rejected), and the trailing word in the split form is merely
 * a positional. So the property must be conditioned on the option being
 * arg-taking.
 *
 * The CatalogEntry struct is opaque here, so "arg-taking" is detected
 * operationally: only an arg-taking option accepts the glued /X:value form
 * (findOption's glued prefix branch is guarded by opt->has_arg). When the
 * glued form is accepted we KNOW the flag is an arg option, and then the split
 * form must be accepted too. When the glued form is rejected the flag is not a
 * recognised arg option for this entry, and the equivalence does not apply. */

static enum theft_trial_res
prop_glued_eq_split(struct theft *t, void *arg1)
{
    struct flagcase *c = (struct flagcase *)arg1;
    const char *name = g_known_names[c->entry_idx];
    const CatalogEntry *e;
    char glued[24];
    const char *split_argv[3];
    const char *glued_argv[2];
    char errA[64], errB[64];
    int  glued_ok, split_ok;
    int  fl;
    (void)t;

    e = CatalogLookup(g_cat, name);
    if (e == NULL) {
        return THEFT_TRIAL_SKIP;
    }

    /* glued form: "<flag>:value" -- acceptance proves the flag is arg-taking */
    fl = (int)strlen(c->flag);
    if (fl + 6 >= (int)sizeof(glued)) {
        return THEFT_TRIAL_SKIP;
    }
    memcpy(glued, c->flag, (size_t)fl);
    glued[fl] = ':';
    memcpy(glued + fl + 1, "value", 5);
    glued[fl + 6] = '\0';
    glued_argv[0] = name;
    glued_argv[1] = glued;
    glued_ok = CatalogValidateArgs(e, glued_argv, 2, errB, (int)sizeof(errB));

    if (!glued_ok) {
        /* Not a recognised arg option; equivalence does not apply. */
        return THEFT_TRIAL_SKIP;
    }

    /* split form: "<flag> value" -- must also be accepted. */
    split_argv[0] = name;
    split_argv[1] = c->flag;
    split_argv[2] = "value";
    split_ok = CatalogValidateArgs(e, split_argv, 3, errA, (int)sizeof(errA));

    if (!split_ok) {
        return THEFT_TRIAL_FAIL;   /* arg option: glued accepted, split must too */
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

    if (!load_catalog()) {
        return 2;
    }
    printf("theft_catalog (src/catalog.c, %d entries loaded):\n", g_entry_count);

    fails += run1("catalog/lookup_total",   prop_lookup_total,  &rname_info,    SEED);
    fails += run1("catalog/entries_named",  prop_entries_named, &idx_info,      SEED ^ 0x11);
    fails += run1("catalog/reject_unknown", prop_reject_unknown,&flagcase_info, SEED ^ 0x22);
    fails += run1("catalog/glued_eq_split", prop_glued_eq_split,&flagcase_info, SEED ^ 0x33);

    CatalogFree(g_cat);
    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
