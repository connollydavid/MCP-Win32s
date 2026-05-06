/*
 * prop.h - minimal property-based testing for VC6 / C89
 *
 * Single header. No shrinking. Random testing with seeded per-trial
 * reproduction and file:line failure reports.
 *
 * Define PROP_IMPLEMENTATION in exactly one .c file before including.
 * Other TUs just include for prototypes and macros.
 *
 * Tests use PROP_TEST(name) { ... } and PROP_CHECK / PROP_FAIL.
 * Failure path is setjmp/longjmp -- no return needed.
 *
 *     PROP_TEST(addition_commutes) {
 *         int a = PROP_INT(-10000, 10000);
 *         int b = PROP_INT(-10000, 10000);
 *         PROP_CHECK(a + b == b + a);
 *     }
 *
 *     int main(void) {
 *         prop_seed(0);
 *         PROP_RUN(addition_commutes, 1000);
 *         return prop_summary();
 *     }
 *
 * Per-trial seeds derived from run seed for reproduction.
 * No C99. No inline. No variadic macros. No long long.
 * xorshift32 state masked to 32 bits for 64-bit hosts.
 */

#ifndef PROP_H
#define PROP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>

#define PROP_MSG_LEN 512

typedef struct prop_ctx_s prop_ctx;
struct prop_ctx_s {
    unsigned long seed;
    unsigned long trial_seed;
    unsigned long rng;
    int           trial;
    char          msg[PROP_MSG_LEN];
    jmp_buf       jb;
};

typedef void (*prop_fn)(prop_ctx *);

/* public API */
void          prop_seed(unsigned long s);
int           prop_run(const char *name, prop_fn fn, int trials);
int           prop_summary(void);

unsigned long prop_rand(prop_ctx *c);
int           prop_int(prop_ctx *c, int lo, int hi);
char          prop_char(prop_ctx *c);
char          prop_char_raw(prop_ctx *c);
void          prop_str(prop_ctx *c, char *buf, int max);
int           prop_choice(prop_ctx *c, int n);
int           prop_bool(prop_ctx *c);

/* test-writing macros */
#define PROP_TEST(name)     static void name(prop_ctx *_pc)
#define PROP_INT(lo, hi)    prop_int(_pc, (lo), (hi))
#define PROP_CHAR()         prop_char(_pc)
#define PROP_CHAR_RAW()     prop_char_raw(_pc)
#define PROP_STR(buf, max)  prop_str(_pc, (buf), (max))
#define PROP_CHOICE(n)      prop_choice(_pc, (n))
#define PROP_BOOL()         prop_bool(_pc)

#define PROP_CHECK(cond)                                                 \
    do {                                                                 \
        if (!(cond)) {                                                   \
            sprintf(_pc->msg, "%s:%d  %s",                               \
                    __FILE__, __LINE__, #cond);                          \
            longjmp(_pc->jb, 1);                                         \
        }                                                                \
    } while (0)

#define PROP_FAIL(reason)                                                \
    do {                                                                 \
        sprintf(_pc->msg, "%s:%d  %s",                                   \
                __FILE__, __LINE__, (reason));                           \
        longjmp(_pc->jb, 1);                                             \
    } while (0)

#define PROP_RUN(fn, trials)  prop_run(#fn, fn, (trials))


#ifdef PROP_IMPLEMENTATION

static unsigned long prop_g_seed   = 0;
static int           prop_g_passed = 0;
static int           prop_g_failed = 0;

void prop_seed(unsigned long s)
{
    if (s == 0) s = (unsigned long)time(NULL);
    if (s == 0) s = 1;
    prop_g_seed = s;
}

unsigned long prop_rand(prop_ctx *c)
{
    unsigned long x = c->rng;
    x ^= x << 13;
    x ^= (x & 0xFFFFFFFFUL) >> 17;
    x ^= x << 5;
    x &= 0xFFFFFFFFUL;
    if (x == 0) x = 1;
    c->rng = x;
    return x;
}

int prop_int(prop_ctx *c, int lo, int hi)
{
    unsigned long r;
    unsigned long range;
    int           t;

    if (lo == hi) return lo;
    if (lo > hi) { t = lo; lo = hi; hi = t; }

    /* edge bias -- one of these about 5/32 of the time */
    r = prop_rand(c);
    switch (r & 0x1F) {
    case 0:  return lo;
    case 1:  return hi;
    case 2:  if (lo <= 0  && 0  <= hi) return 0;  break;
    case 3:  if (lo <= 1  && 1  <= hi) return 1;  break;
    case 4:  if (lo <= -1 && -1 <= hi) return -1; break;
    default: break;
    }

    range = (unsigned long)hi - (unsigned long)lo + 1UL;
    if (range == 0) return (int)prop_rand(c);
    return lo + (int)(prop_rand(c) % range);
}

char prop_char(prop_ctx *c)
{
    return (char)(0x20 + (prop_rand(c) % 95UL));
}

char prop_char_raw(prop_ctx *c)
{
    return (char)(prop_rand(c) & 0xFF);
}

void prop_str(prop_ctx *c, char *buf, int max)
{
    int n, i;
    if (max <= 0) return;
    if (max == 1) { buf[0] = '\0'; return; }
    n = (int)(prop_rand(c) % (unsigned long)(max - 1));
    for (i = 0; i < n; i++) buf[i] = prop_char(c);
    buf[n] = '\0';
}

int prop_choice(prop_ctx *c, int n)
{
    if (n <= 1) return 0;
    return (int)(prop_rand(c) % (unsigned long)n);
}

int prop_bool(prop_ctx *c)
{
    return (int)(prop_rand(c) & 1UL);
}

int prop_run(const char *name, prop_fn fn, int trials)
{
    prop_ctx c;
    int      t;

    if (prop_g_seed == 0) prop_seed(0);
    c.seed = prop_g_seed;

    printf("[ RUN  ] %s  seed=%lu  trials=%d\n", name, c.seed, trials);

    for (t = 0; t < trials; t++) {
        c.trial      = t;
        c.trial_seed = c.seed ^ ((unsigned long)t * 2654435761UL);
        c.trial_seed &= 0xFFFFFFFFUL;
        if (c.trial_seed == 0) c.trial_seed = 1;
        c.rng        = c.trial_seed;
        c.msg[0]     = '\0';

        if (setjmp(c.jb) == 0) {
            fn(&c);
        } else {
            printf("[ FAIL ] %s  trial=%d  trial_seed=%lu\n",
                   name, t, c.trial_seed);
            printf("         %s\n", c.msg);
            printf("         reproduce: prop_seed(%lu); run trial %d\n",
                   c.seed, t);
            prop_g_failed++;
            return 0;
        }
    }

    printf("[ PASS ] %s  %d trials\n", name, trials);
    prop_g_passed++;
    return 1;
}

int prop_summary(void)
{
    printf("---\n%d passed, %d failed\n", prop_g_passed, prop_g_failed);
    return prop_g_failed == 0 ? 0 : 1;
}

#endif /* PROP_IMPLEMENTATION */
#endif /* PROP_H */
