/*
 * test_strutil.c - prop.h on-target mirror of the McpStrCpyN properties
 * (src/strutil.c, the device's own DBCS-aware bounded copy - the NT 3.1
 * floor lacks lstrcpynA).
 *
 * Mirrors the theft host properties (tests/host/theft_strutil.c) at lower
 * trial counts so the bounded/NUL/prefix/no-split contract is proven on the
 * actual shipped C89/i386 build, not only natively. Here CharNextA is the
 * REAL Win32 one - single-byte on the build codepage - so the no-split
 * property holds trivially on this host; the test still pins the C89 build's
 * bounded-write, always-NUL-terminated and byte-exact-prefix behaviour and
 * the n <= 0 / dst == NULL guards.
 *
 * Uses prop.h (minimal C89 PBT framework, fixed seeds, lower trial counts).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#define PROP_IMPLEMENTATION
#include "prop.h"
#include "strutil.h"
#include <windows.h>     /* CharNextA */
#include <stdio.h>
#include <string.h>

#define MAX_SRC 128

/* Fill buf with a random NUL-terminated src of length 0..MAX_SRC-1, using
 * non-NUL bytes so the intended length is realised. Returns the length. */
static int gen_src(prop_ctx *_pc, char *buf)
{
    int len, i;
    len = PROP_INT(0, MAX_SRC - 1);
    for (i = 0; i < len; i++) {
        int b;
        b = PROP_INT(1, 255);   /* never an interior NUL */
        buf[i] = (char)b;
    }
    buf[len] = '\0';
    return len;
}

/* P1: n > 0 => result is NUL-terminated and strlen(result) <= n-1. */
PROP_TEST(strutil_terminated) {
    char src[MAX_SRC + 1];
    char dst[MAX_SRC + 1];
    int n, rlen;

    gen_src(_pc, src);
    n = PROP_INT(1, MAX_SRC + 1);
    memset(dst, 'Z', sizeof(dst));
    McpStrCpyN(dst, src, n);
    rlen = (int)strlen(dst);          /* terminates: strlen stops at the NUL */
    PROP_CHECK(rlen <= n - 1);
}

/* P2: result bytes are a byte-exact prefix of src. */
PROP_TEST(strutil_prefix) {
    char src[MAX_SRC + 1];
    char dst[MAX_SRC + 1];
    int n, rlen, srclen;

    srclen = gen_src(_pc, src);
    n = PROP_INT(1, MAX_SRC + 1);
    McpStrCpyN(dst, src, n);
    rlen = (int)strlen(dst);
    PROP_CHECK(rlen <= srclen);
    PROP_CHECK(memcmp(dst, src, (size_t)rlen) == 0);
}

/* P3: re-walking the result with CharNextA lands EXACTLY on the terminator
 * (never splits a character; trivially holds on a single-byte build cp). */
PROP_TEST(strutil_no_split) {
    char src[MAX_SRC + 1];
    char dst[MAX_SRC + 1];
    const char *p;
    int n;

    gen_src(_pc, src);
    n = PROP_INT(1, MAX_SRC + 1);
    McpStrCpyN(dst, src, n);
    p = dst;
    while (*p != '\0') {
        const char *q;
        q = CharNextA(p);
        PROP_CHECK(q != p);           /* always makes progress */
        p = q;
    }
    /* arrived exactly at the terminator */
    PROP_CHECK(*p == '\0');
}

/* P4: bounded write - allocate dst as EXACTLY n bytes with a poisoned guard
 * byte immediately after, and assert McpStrCpyN never disturbs the guard
 * (never writes at or beyond dst[n]). */
PROP_TEST(strutil_bounded) {
    char src[MAX_SRC + 1];
    char buf[MAX_SRC + 2];
    int n;

    gen_src(_pc, src);
    n = PROP_INT(1, MAX_SRC);
    memset(buf, 'Z', sizeof(buf));
    buf[n] = (char)0xAB;              /* guard byte right after the n-byte dst */
    McpStrCpyN(buf, src, n);
    PROP_CHECK((unsigned char)buf[n] == 0xAB);   /* guard intact */
    PROP_CHECK((int)strlen(buf) <= n - 1);       /* NUL within the n bytes */
}

/* P5: edges - dst == NULL returns NULL; n <= 0 writes nothing. */
PROP_TEST(strutil_edges) {
    char src[MAX_SRC + 1];
    char buf[4];
    int n;
    char *r;

    gen_src(_pc, src);

    /* dst == NULL returns NULL for any n. */
    n = PROP_INT(-8, MAX_SRC);
    r = McpStrCpyN(NULL, src, n);
    PROP_CHECK(r == NULL);

    /* n <= 0 writes nothing and returns dst unchanged. */
    buf[0] = (char)0xAB;
    n = PROP_INT(-8, 0);              /* in -8..0 */
    r = McpStrCpyN(buf, src, n);
    PROP_CHECK(r == buf);
    PROP_CHECK((unsigned char)buf[0] == 0xAB);
}

int main(void)
{
    prop_seed(0);

    PROP_RUN(strutil_terminated, 2000);
    PROP_RUN(strutil_prefix,     2000);
    PROP_RUN(strutil_no_split,   2000);
    PROP_RUN(strutil_bounded,    2000);
    PROP_RUN(strutil_edges,      2000);

    return prop_summary();
}
