/*
 * test_audit.c - Unit tests for audit.c (the fail-closed memory-write log)
 *
 * Obligations discharged (see tests/OBLIGATIONS-5.3.md):
 *   entity-fields.AuditRecord                      - audit_record_shape
 *   invariant.PokeIsAuditedFailClosed (sink half)  - audit_unwritable_fails,
 *                                                    audit_disarms_when_unwritable
 *   rule-failure.MemoryPoked.1 (device arm)        - audit_arm_state
 *
 * The arm (/ALLOWMEMWRITE) and the sink writability are the two device-side
 * gates the poke path consults; this suite pins them in isolation. The
 * poke->audit fail-closed coupling itself is pinned in test_mem_ops.c.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "audit.h"

/* A writable sink in the temp directory; an unwritable sink under a
 * directory that does not exist (CreateFileA OPEN_ALWAYS fails). */
static char g_writable[MAX_PATH];
static char g_unwritable[MAX_PATH];

static void make_paths(void)
{
    char tmp[MAX_PATH];
    DWORD n;

    n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0) {
        lstrcpynA(tmp, ".\\", (int)sizeof(tmp));
    }
    lstrcpynA(g_writable, tmp, (int)sizeof(g_writable));
    lstrcatA(g_writable, "mcp_audit_test.log");
    DeleteFileA(g_writable);

    /* A path whose parent directory does not exist -> not writable. */
    lstrcpynA(g_unwritable, tmp, (int)sizeof(g_unwritable));
    lstrcatA(g_unwritable, "no_such_dir_mcp\\audit.log");
}

/* Read a whole file into buf (NUL-terminated). Returns bytes read, -1 on
 * failure. */
static int read_file(const char *path, char *buf, int bufSize)
{
    HANDLE h;
    DWORD got;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!ReadFile(h, buf, (DWORD)(bufSize - 1), &got, NULL)) {
        CloseHandle(h);
        return -1;
    }
    buf[got] = '\0';
    CloseHandle(h);
    return (int)got;
}

static int count_lines(const char *s)
{
    int n;
    int i;

    n = 0;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == '\n') {
            n++;
        }
    }
    return n;
}

/* ========================================================
 * #1 Default state is disarmed; a no-arm configure stays off.
 * ======================================================== */
TEST_CASE(audit_default_disarmed) {
    make_paths();
    TEST_ASSERT_INT_EQUAL(0, AuditConfigure(0, g_writable),
                          "no /ALLOWMEMWRITE -> disarmed");
    TEST_ASSERT_INT_EQUAL(0, AuditIsArmed(), "AuditIsArmed reflects it");
}

/* ========================================================
 * #2 Arm requested + writable sink -> armed (rule-failure.MemoryPoked.1
 *    device-arm half).
 * ======================================================== */
TEST_CASE(audit_arm_state) {
    make_paths();
    TEST_ASSERT_INT_EQUAL(1, AuditConfigure(1, g_writable),
                          "arm + writable -> armed");
    TEST_ASSERT_INT_EQUAL(1, AuditIsArmed(), "AuditIsArmed reflects it");
}

/* ========================================================
 * #3 Arm requested but the sink is NOT writable -> DISARMED
 *    (fail-closed startup check: a write that cannot be logged
 *    must never be armable). invariant.PokeIsAuditedFailClosed.
 * ======================================================== */
TEST_CASE(audit_disarms_when_unwritable) {
    make_paths();
    TEST_ASSERT_INT_EQUAL(0, AuditConfigure(1, g_unwritable),
                          "arm + unwritable -> disarmed (fail-closed)");
    TEST_ASSERT_INT_EQUAL(0, AuditIsArmed(), "stays disarmed");
}

/* ========================================================
 * #4 A poke record is appended durably; two writes -> two lines;
 *    the line carries the AuditRecord fields (entity-fields.AuditRecord).
 * ======================================================== */
TEST_CASE(audit_record_shape) {
    char buf[2048];
    int n;

    make_paths();
    AuditConfigure(1, g_writable);

    TEST_ASSERT_INT_EQUAL(1,
        AuditWritePoke("process", "m1-742", 4242, "cl /c x.c",
                       0x00401000UL, 64UL, 64UL, 0),
        "first record written");
    TEST_ASSERT_INT_EQUAL(1,
        AuditWritePoke("process", "m2-918", 4242, "cl /c x.c",
                       0x00401040UL, 16UL, 8UL, 1),
        "second record written");

    n = read_file(g_writable, buf, (int)sizeof(buf));
    TEST_ASSERT(n > 0, "log file readable");
    TEST_ASSERT_INT_EQUAL(2, count_lines(buf), "exactly two records appended");
    TEST_ASSERT(strstr(buf, "POKE") != NULL, "record marks POKE");
    TEST_ASSERT(strstr(buf, "token=m1-742") != NULL, "first token logged");
    TEST_ASSERT(strstr(buf, "addr=0x00401000") != NULL, "address logged in hex");
    TEST_ASSERT(strstr(buf, "len=64") != NULL, "length logged");
    TEST_ASSERT(strstr(buf, "written=8") != NULL, "second bytes_written logged");
    TEST_ASSERT(strstr(buf, "partial=1") != NULL, "partial flag logged");
}

/* ========================================================
 * #5 When the sink is unwritable, a poke record cannot be written:
 *    AuditWritePoke returns 0 so the caller refuses the poke (no
 *    unlogged mutation). invariant.PokeIsAuditedFailClosed.
 * ======================================================== */
TEST_CASE(audit_unwritable_fails) {
    make_paths();
    AuditConfigure(1, g_unwritable);
    TEST_ASSERT_INT_EQUAL(0,
        AuditWritePoke("process", "m1-1", 1, "cl",
                       0x1000UL, 4UL, 4UL, 0),
        "an unwritable sink fails the record write");
}

int main(void)
{
    printf("Running audit.c tests...\n\n");
    RUN_TEST(audit_default_disarmed);
    RUN_TEST(audit_arm_state);
    RUN_TEST(audit_disarms_when_unwritable);
    RUN_TEST(audit_record_shape);
    RUN_TEST(audit_unwritable_fails);
    print_test_summary();
    return g_tests_failed;
}
