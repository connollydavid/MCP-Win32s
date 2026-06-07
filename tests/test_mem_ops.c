/*
 * test_mem_ops.c - Unit tests for src/mem_ops.c (memory peek/poke subsystem)
 *
 * Obligations discharged (see tests/OBLIGATIONS-5.3.md):
 *   invariant.AddressIsWellFormed          (PIN #1) - mem_parse_u32_*
 *   invariant.MemoryAccessRangeBounded     (PIN #2) - range_guard_*,
 *                                                     range_cap_default
 *   invariant.PreNtAccessGuarded           (PIN #3) - region_accessible_decision
 *   enum-comparable.MemTier                          - os_family_maps_to_tier
 *   entity-fields.RetainedProcess,
 *     transition-edge/terminal/rejected.RetainedProcess,
 *     invariant.RetainedTokenValid         (PIN #4) - token_table_lifecycle
 *   (table exhaustion + leak)                        - process_table_bounded
 *   rule-failure.ProcessRetained.1 +
 *     invariant.SpawnRetainCommandIsCatalogued (PIN #5)
 *                                                    - spawn_retain_requires_catalogued
 *   rule-success.MemoryPeeked/MemoryPoked            - peek_process_tier_round_trip
 *   rule-failure.MemoryPoked.1             (PIN #7 device half)
 *                                                    - poke_requires_device_arm
 *   invariant.PokeIsAuditedFailClosed      (PIN #6) - poke_fail_closed_audit
 *
 * The live pre-NT arena/shared_vm load/store path is never taken on the NT dev
 * host or CI (the tier is `process` there), so it is NOT exercised live here -
 * region_accessible_decision covers the guard logic against a synthetic MBI,
 * and live pre-NT verification waits for real pre-NT hardware (the
 * OBLIGATIONS-5.3.md recorded non-obligation; the test_pty_exec.c
 * skip-with-reason convention).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef TEST_BUILD
#define TEST_BUILD
#endif

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "mem_ops.h"
#include "feat.h"
#include "audit.h"
#include "catalog.h"

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

/* Locate mem_target.exe next to this test executable (model: test_argv.c). */
static int LocateMemTarget(char *path, int pathSize)
{
    char self[MAX_PATH];
    DWORD len;
    int i, lastSlash;

    len = GetModuleFileNameA(NULL, self, sizeof(self));
    if (len == 0 || len >= sizeof(self)) {
        return 0;
    }
    lastSlash = -1;
    for (i = 0; self[i] != '\0'; i++) {
        if (self[i] == '\\' || self[i] == '/') {
            lastSlash = i;
        }
    }
    if (lastSlash < 0) {
        return 0;
    }
    self[lastSlash + 1] = '\0';
    if ((int)(lstrlen(self) + lstrlen("mem_target.exe")) >= pathSize) {
        return 0;
    }
    lstrcpy(path, self);
    lstrcat(path, "mem_target.exe");
    return 1;
}

/* A writable audit sink in the temp directory. */
static void ArmAudit(void)
{
    char tmp[MAX_PATH];
    char sink[MAX_PATH];
    DWORD n;

    n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0) {
        lstrcpynA(tmp, ".\\", (int)sizeof(tmp));
    }
    lstrcpynA(sink, tmp, (int)sizeof(sink));
    lstrcatA(sink, "mcp_mem_test_audit.log");
    DeleteFileA(sink);
    AuditConfigure(1, sink);
}

/* ========================================================
 * MemParseU32 - SAFETY PIN #1 (invariant.AddressIsWellFormed)
 * ======================================================== */

TEST_CASE(mem_parse_u32_accepts) {
    unsigned long v;

    TEST_ASSERT(MemParseU32("0x401000", &v) == 1, "hex prefix parses");
    TEST_ASSERT(v == 0x401000UL, "hex value correct");
    TEST_ASSERT(MemParseU32("64", &v) == 1, "decimal parses");
    TEST_ASSERT(v == 64UL, "decimal value correct");
    TEST_ASSERT(MemParseU32("0", &v) == 1, "zero parses");
    TEST_ASSERT(v == 0UL, "zero value correct");
    TEST_ASSERT(MemParseU32("0xFFFFFFFF", &v) == 1, "max 32-bit hex parses");
    TEST_ASSERT(v == 0xFFFFFFFFUL, "max value correct");
    TEST_ASSERT(MemParseU32("0X1A", &v) == 1, "uppercase 0X prefix parses");
    TEST_ASSERT(v == 0x1AUL, "uppercase hex value correct");
}

TEST_CASE(mem_parse_u32_rejects) {
    unsigned long v;

    v = 0xDEADBEEFUL;
    TEST_ASSERT(MemParseU32("", &v) == 0, "empty rejected");
    TEST_ASSERT(MemParseU32("xyz", &v) == 0, "garbage rejected");
    TEST_ASSERT(MemParseU32("12x", &v) == 0, "trailing non-digit rejected");
    TEST_ASSERT(MemParseU32("0x100000000", &v) == 0, "33-bit overflow rejected");
    TEST_ASSERT(MemParseU32("4294967296", &v) == 0,
                "decimal overflow rejected");
    TEST_ASSERT(MemParseU32("-1", &v) == 0, "negative rejected");
    TEST_ASSERT(MemParseU32("0x", &v) == 0, "bare 0x rejected");
    TEST_ASSERT(v == 0xDEADBEEFUL, "out untouched on rejection");
}

/* ========================================================
 * MemRangeInBounds - SAFETY PIN #2 (invariant.MemoryAccessRangeBounded)
 * ======================================================== */

TEST_CASE(range_guard_ok) {
    TEST_ASSERT(MemRangeInBounds(0x1000UL, 64UL, 65536UL) == 1,
                "in-range access admitted");
    TEST_ASSERT(MemRangeInBounds(0x1000UL, 0UL, 65536UL) == 1,
                "zero-length access admitted");
}

TEST_CASE(range_guard_wraparound) {
    TEST_ASSERT(MemRangeInBounds(0xFFFFFFF0UL, 0x100UL, 65536UL) == 0,
                "addr+len wrapping past 0xFFFFFFFF rejected");
}

TEST_CASE(range_guard_cap) {
    TEST_ASSERT(MemRangeInBounds(0UL, 65537UL, 65536UL) == 0,
                "length above cap rejected");
}

TEST_CASE(range_cap_default) {
    TEST_ASSERT(MemRangeInBounds(0x1000UL, MEM_MAX_ACCESS, MEM_MAX_ACCESS) == 1,
                "length == cap admitted");
    TEST_ASSERT(MemRangeInBounds(0x1000UL, MEM_MAX_ACCESS + 1UL,
                                 MEM_MAX_ACCESS) == 0,
                "length above MEM_MAX_ACCESS rejected");
}

/* ========================================================
 * MemRegionAccessibleDecision - SAFETY PIN #3 (PreNtAccessGuarded)
 * Synthetic MBIs - no real memory touched.
 * ======================================================== */

TEST_CASE(region_accessible_decision) {
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long base;

    base = 0x10000UL;

    /* Committed PAGE_READWRITE, request fully inside -> len. */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x1000;
    mbi.State = MEM_COMMIT;
    mbi.Protect = PAGE_READWRITE;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 0) == 64UL,
                "committed RW read: full prefix");
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 1) == 64UL,
                "committed RW write: full prefix");

    /* MEM_FREE -> 0. */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x1000;
    mbi.State = MEM_FREE;
    mbi.Protect = PAGE_NOACCESS;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 0) == 0UL,
                "MEM_FREE: nothing accessible");

    /* MEM_RESERVE -> 0. */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x1000;
    mbi.State = MEM_RESERVE;
    mbi.Protect = PAGE_READWRITE;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 0) == 0UL,
                "MEM_RESERVE: nothing accessible");

    /* PAGE_NOACCESS committed -> 0. */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x1000;
    mbi.State = MEM_COMMIT;
    mbi.Protect = PAGE_NOACCESS;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 0) == 0UL,
                "PAGE_NOACCESS: nothing accessible");

    /* Committed PAGE_READONLY: readable, but write -> 0. */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x1000;
    mbi.State = MEM_COMMIT;
    mbi.Protect = PAGE_READONLY;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 0) == 64UL,
                "PAGE_READONLY read: accessible");
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 1) == 0UL,
                "PAGE_READONLY write: rejected");

    /* A region that ends before addr+len -> partial prefix (truncated). */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x100;                 /* ends at base + 0x100 */
    mbi.State = MEM_COMMIT;
    mbi.Protect = PAGE_READWRITE;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 0x200UL, 0) == 0x100UL,
                "spanning read clamps to accessible prefix");

    /* PAGE_GUARD -> 0 even if committed + RW. */
    memset(&mbi, 0, sizeof(mbi));
    mbi.BaseAddress = (PVOID)base;
    mbi.RegionSize = 0x1000;
    mbi.State = MEM_COMMIT;
    mbi.Protect = PAGE_READWRITE | PAGE_GUARD;
    TEST_ASSERT(MemRegionAccessibleDecision(&mbi, base, 64UL, 0) == 0UL,
                "PAGE_GUARD: nothing accessible");
}

/* ========================================================
 * MemTierCurrent / MemTierName - enum-comparable.MemTier
 * ======================================================== */

TEST_CASE(os_family_maps_to_tier) {
    int saved_nt, saved_9x, saved_32s;

    FeatInit();
    saved_nt = g_features.is_nt;
    saved_9x = g_features.is_win9x;
    saved_32s = g_features.is_win32s;

    g_features.is_nt = 1; g_features.is_win9x = 0; g_features.is_win32s = 0;
    TEST_ASSERT_STR_EQUAL("process", MemTierName(MemTierCurrent()),
                          "is_nt -> process");

    g_features.is_nt = 0; g_features.is_win9x = 1; g_features.is_win32s = 0;
    TEST_ASSERT_STR_EQUAL("arena", MemTierName(MemTierCurrent()),
                          "is_win9x -> arena");

    g_features.is_nt = 0; g_features.is_win9x = 0; g_features.is_win32s = 1;
    TEST_ASSERT_STR_EQUAL("shared_vm", MemTierName(MemTierCurrent()),
                          "is_win32s -> shared_vm");

    g_features.is_nt = 0; g_features.is_win9x = 0; g_features.is_win32s = 0;
    TEST_ASSERT_STR_EQUAL("none", MemTierName(MemTierCurrent()),
                          "no family -> none");

    g_features.is_nt = saved_nt;
    g_features.is_win9x = saved_9x;
    g_features.is_win32s = saved_32s;
}

/* ========================================================
 * The spawn-retain token table - PIN #4 (RetainedTokenValid) +
 * entity/transition obligations.
 * ======================================================== */

TEST_CASE(token_table_lifecycle) {
    char target[MAX_PATH];
    const char *argv[1];
    MemSpawnResult r1, r2;
    char tok1[MEM_TOKEN_LEN];

    MemResetForTest();
    TEST_ASSERT(LocateMemTarget(target, (int)sizeof(target)),
                "mem_target.exe located");
    argv[0] = target;

    /* spawnRetain (unsafeMode=1) -> ok, non-empty token, handle resolves. */
    MemSpawnRetain(NULL, 1, argv, 1, &r1);
    TEST_ASSERT(r1.ok == 1, "spawn-retain succeeds (unsafe)");
    TEST_ASSERT(r1.token[0] != '\0', "token non-empty");
    TEST_ASSERT(r1.pid != 0, "pid recorded");
    TEST_ASSERT(MemTokenHandle(r1.token) != NULL, "handle resolves while live");
    lstrcpynA(tok1, r1.token, (int)sizeof(tok1));

    /* MemRelease consumes the token (handle no longer resolves). */
    TEST_ASSERT(MemRelease(tok1) == 1, "release on a live token succeeds");
    TEST_ASSERT(MemTokenHandle(tok1) == NULL,
                "handle no longer resolves after release");
    /* A second release fails (terminal: released). */
    TEST_ASSERT(MemRelease(tok1) == 0, "second release on consumed token fails");

    /* A forged / never-issued token resolves to NULL. */
    TEST_ASSERT(MemTokenHandle("m999-12345") == NULL,
                "forged token resolves to NULL");

    /* Terminate on a fresh spawn consumes it too. */
    MemSpawnRetain(NULL, 1, argv, 1, &r2);
    TEST_ASSERT(r2.ok == 1, "second spawn succeeds");
    /* Tokens never reused: two spawns -> different tokens. */
    TEST_ASSERT(strcmp(r2.token, tok1) != 0,
                "second token differs from the first (never reused)");
    TEST_ASSERT(MemTerminate(r2.token) == 1, "terminate on live token succeeds");
    TEST_ASSERT(MemTokenHandle(r2.token) == NULL,
                "handle gone after terminate");
    TEST_ASSERT(MemTerminate(r2.token) == 0,
                "second terminate on consumed token fails");

    MemReleaseAll();
}

/* ========================================================
 * Table exhaustion + leak (process_table_bounded)
 * ======================================================== */

TEST_CASE(process_table_bounded) {
    char target[MAX_PATH];
    const char *argv[1];
    MemSpawnResult r;
    int i;
    int spawned;

    MemResetForTest();
    TEST_ASSERT(LocateMemTarget(target, (int)sizeof(target)),
                "mem_target.exe located");
    argv[0] = target;

    spawned = 0;
    for (i = 0; i < MEM_PROC_SLOTS; i++) {
        MemSpawnRetain(NULL, 1, argv, 1, &r);
        TEST_ASSERT(r.ok == 1, "each of the 8 slots fills");
        spawned++;
    }
    TEST_ASSERT_INT_EQUAL(MEM_PROC_SLOTS, spawned, "all slots filled");

    /* The 9th fails with no eviction. */
    MemSpawnRetain(NULL, 1, argv, 1, &r);
    TEST_ASSERT(r.ok == 0, "9th spawn refused (table full)");
    TEST_ASSERT(strstr(r.reason, "full") != NULL, "reason says table full");

    /* MemReleaseAll frees the table (no leaked handles). */
    MemReleaseAll();
    /* A spawn now succeeds again - the table was cleared. */
    MemSpawnRetain(NULL, 1, argv, 1, &r);
    TEST_ASSERT(r.ok == 1, "spawn succeeds again after releaseAll");
    MemReleaseAll();
}

/* ========================================================
 * spawn_retain_requires_catalogued - SAFETY PIN #5
 * ======================================================== */

/* Write a tiny catalog JSON that contains exactly one external command
 * ("knowncmd") and one shell-builtin ("dir"), neither of which is the
 * mem_target path - so an enforced gate refuses the target. */
static int WriteTinyCatalog(char *pathOut, int pathSize)
{
    char tmp[MAX_PATH];
    DWORD n;
    HANDLE h;
    DWORD wrote;
    static const char *json =
        "{ \"version\": 1, \"commands\": {"
        " \"knowncmd\": { \"kind\": \"external\","
        " \"supports_win32s\": true, \"options\": [], \"positional\": [] },"
        " \"dir\": { \"kind\": \"shell-builtin\","
        " \"shell_modern\": \"cmd.exe /c dir\","
        " \"shell_win32s\": \"command.com /c dir\","
        " \"supports_win32s\": true, \"options\": [], \"positional\": [] }"
        " } }";

    n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0) {
        lstrcpynA(tmp, ".\\", (int)sizeof(tmp));
    }
    lstrcpynA(pathOut, tmp, pathSize);
    lstrcatA(pathOut, "mcp_mem_tiny_catalog.json");

    h = CreateFileA(pathOut, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    WriteFile(h, json, (DWORD)lstrlenA(json), &wrote, NULL);
    CloseHandle(h);
    return 1;
}

TEST_CASE(spawn_retain_requires_catalogued) {
    char catPath[MAX_PATH];
    char target[MAX_PATH];
    Catalog *cat;
    char err[256];
    const char *argv[1];
    const char *builtinArgv[1];
    MemSpawnResult r;

    MemResetForTest();
    TEST_ASSERT(WriteTinyCatalog(catPath, (int)sizeof(catPath)),
                "tiny catalog written");
    TEST_ASSERT(CatalogLoad(catPath, &cat, err, (int)sizeof(err)) == 1,
                "tiny catalog loads");
    TEST_ASSERT(LocateMemTarget(target, (int)sizeof(target)),
                "mem_target.exe located");

    /* The mem_target path is not in the catalog -> refused (enforced gate). */
    argv[0] = target;
    MemSpawnRetain(cat, 0, argv, 1, &r);
    TEST_ASSERT(r.ok == 0, "uncatalogued command refused");
    TEST_ASSERT(strstr(r.reason, "catalog") != NULL,
                "reason cites the catalog");

    /* A shell-builtin ("dir") is also refused (no process to retain). */
    builtinArgv[0] = "dir";
    MemSpawnRetain(cat, 0, builtinArgv, 1, &r);
    TEST_ASSERT(r.ok == 0, "shell builtin refused as a spawn-retain target");
    TEST_ASSERT(strstr(r.reason, "builtin") != NULL,
                "reason cites the builtin");

    CatalogFree(cat);
    DeleteFileA(catPath);
}

/* ========================================================
 * peek_process_tier_round_trip - rule-success.MemoryPeeked/MemoryPoked
 * (CI-executable: NT process tier).
 * ======================================================== */

/* Resolve VirtualQueryEx ourselves (NT-only; this test only runs on NT). */
typedef SIZE_T (WINAPI *VqxFn)(HANDLE, LPCVOID, PMEMORY_BASIC_INFORMATION,
                               SIZE_T);

/* Find the first committed + writable region in the child, returning its base
 * address (the page after the very first page, to avoid the reserved low end).
 * Returns 0 if none found. */
static unsigned long FindWritableRegion(HANDLE child)
{
    VqxFn vqx;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long addr;

    vqx = (VqxFn)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                "VirtualQueryEx");
    if (vqx == NULL) {
        return 0;
    }
    addr = 0x10000UL;
    while (addr < 0x7FFF0000UL) {
        memset(&mbi, 0, sizeof(mbi));
        if (vqx(child, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) {
            break;
        }
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect == PAGE_READWRITE ||
             mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            mbi.RegionSize >= 0x1000) {
            return (unsigned long)mbi.BaseAddress;
        }
        addr = (unsigned long)mbi.BaseAddress +
               (unsigned long)mbi.RegionSize;
    }
    return 0;
}

TEST_CASE(peek_process_tier_round_trip) {
    char target[MAX_PATH];
    const char *argv[1];
    MemSpawnResult sr;
    MemPokeResult pr;
    MemPeekResult pk;
    HANDLE child;
    unsigned long addr;
    unsigned char pattern[16];
    unsigned char readback[16];
    int i;

    /* This path is the NT process tier (the dev host + CI are NT). */
    FeatInit();
    if (!g_features.is_nt) {
        /* SKIP-with-reason: the pre-NT live round-trip needs pre-NT hardware. */
        printf("(skipped: non-NT host; pre-NT live verification deferred) ");
        return;
    }

    MemResetForTest();
    ArmAudit();
    TEST_ASSERT(LocateMemTarget(target, (int)sizeof(target)),
                "mem_target.exe located");
    argv[0] = target;

    MemSpawnRetain(NULL, 1, argv, 1, &sr);
    TEST_ASSERT(sr.ok == 1, "spawn-retain succeeds");

    child = MemTokenHandle(sr.token);
    TEST_ASSERT(child != NULL, "handle resolves");

    /* Let the child finish committing its address space before probing it -
     * an early region can read back zero while the loader is still settling. */
    Sleep(300);
    addr = FindWritableRegion(child);
    TEST_ASSERT(addr != 0, "found a writable region in the child");

    for (i = 0; i < 16; i++) {
        pattern[i] = (unsigned char)(0xA0 + i);
    }

    MemPoke(sr.token, addr, pattern, 16UL, &pr);
    TEST_ASSERT(pr.ok == 1, "poke succeeds");
    TEST_ASSERT(pr.has_process == 1, "poke is process-tier");
    TEST_ASSERT_INT_EQUAL(16, (int)pr.bytes_written, "16 bytes written");

    memset(readback, 0, sizeof(readback));
    MemPeek(sr.token, addr, 16UL, readback, &pk);
    TEST_ASSERT(pk.ok == 1, "peek succeeds");
    TEST_ASSERT(pk.has_process == 1, "peek is process-tier");
    TEST_ASSERT_INT_EQUAL(16, (int)pk.bytes_read, "16 bytes read");
    TEST_ASSERT(memcmp(pattern, readback, 16) == 0,
                "peeked bytes match the poked pattern");

    MemTerminate(sr.token);
    MemReleaseAll();
}

/* ========================================================
 * poke_requires_device_arm - SAFETY PIN #7 (device half)
 * ======================================================== */

TEST_CASE(poke_requires_device_arm) {
    char target[MAX_PATH];
    const char *argv[1];
    MemSpawnResult sr;
    MemPokeResult pr;
    HANDLE child;
    unsigned long addr;
    unsigned char pattern[16];

    FeatInit();
    if (!g_features.is_nt) {
        printf("(skipped: non-NT host) ");
        return;
    }

    MemResetForTest();
    /* Disarm: arm requested = 0. */
    AuditConfigure(0, NULL);
    TEST_ASSERT_INT_EQUAL(0, AuditIsArmed(), "audit disarmed");

    TEST_ASSERT(LocateMemTarget(target, (int)sizeof(target)),
                "mem_target.exe located");
    argv[0] = target;
    MemSpawnRetain(NULL, 1, argv, 1, &sr);
    TEST_ASSERT(sr.ok == 1, "spawn-retain succeeds");
    child = MemTokenHandle(sr.token);
    Sleep(300);
    addr = FindWritableRegion(child);
    TEST_ASSERT(addr != 0, "found a writable region");

    memset(pattern, 0x5A, sizeof(pattern));
    MemPoke(sr.token, addr, pattern, 16UL, &pr);
    TEST_ASSERT(pr.ok == 0, "poke refused when disarmed");
    TEST_ASSERT_INT_EQUAL(0, (int)pr.bytes_written, "no bytes written");
    TEST_ASSERT(strstr(pr.reason, "armed") != NULL, "reason cites the arm");

    MemTerminate(sr.token);
    MemReleaseAll();
}

/* ========================================================
 * poke_fail_closed_audit - SAFETY PIN #6 (PokeIsAuditedFailClosed)
 * An unwritable audit sink ⇒ the poke is refused AND child memory is
 * UNCHANGED (we peek it back and confirm the prior pattern survives).
 * ======================================================== */

TEST_CASE(poke_fail_closed_audit) {
    char target[MAX_PATH];
    char tmp[MAX_PATH];
    char unwritable[MAX_PATH];
    const char *argv[1];
    MemSpawnResult sr;
    MemPokeResult pr;
    MemPeekResult pk;
    HANDLE child;
    unsigned long addr;
    unsigned char first[16];
    unsigned char second[16];
    unsigned char readback[16];
    DWORD n;
    int i;

    FeatInit();
    if (!g_features.is_nt) {
        printf("(skipped: non-NT host) ");
        return;
    }

    MemResetForTest();
    ArmAudit();      /* armed + writable */

    TEST_ASSERT(LocateMemTarget(target, (int)sizeof(target)),
                "mem_target.exe located");
    argv[0] = target;
    MemSpawnRetain(NULL, 1, argv, 1, &sr);
    TEST_ASSERT(sr.ok == 1, "spawn-retain succeeds");
    child = MemTokenHandle(sr.token);
    Sleep(300);
    addr = FindWritableRegion(child);
    TEST_ASSERT(addr != 0, "found a writable region");

    /* Write a known first pattern while the sink is writable. */
    for (i = 0; i < 16; i++) {
        first[i] = (unsigned char)(0x10 + i);
    }
    MemPoke(sr.token, addr, first, 16UL, &pr);
    TEST_ASSERT(pr.ok == 1, "first (audited) poke succeeds");

    /* Point the audit sink at an unwritable path (parent dir absent). This
     * disarms the arm at configure time AND leaves the sink unwritable - both
     * make the next poke fail closed (no write). */
    n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0) {
        lstrcpynA(tmp, ".\\", (int)sizeof(tmp));
    }
    lstrcpynA(unwritable, tmp, (int)sizeof(unwritable));
    lstrcatA(unwritable, "no_such_dir_mem\\audit.log");
    AuditConfigure(1, unwritable);

    /* Attempt a SECOND poke with a different pattern - must be refused. */
    memset(second, 0xFF, sizeof(second));
    MemPoke(sr.token, addr, second, 16UL, &pr);
    TEST_ASSERT(pr.ok == 0, "poke refused when the audit sink is unwritable");
    TEST_ASSERT_INT_EQUAL(0, (int)pr.bytes_written, "no bytes written");

    /* Re-arm a writable sink so we can peek (peek needs no arm, but keep the
     * subsystem consistent), then confirm child memory was NOT mutated. */
    ArmAudit();
    memset(readback, 0, sizeof(readback));
    MemPeek(sr.token, addr, 16UL, readback, &pk);
    TEST_ASSERT(pk.ok == 1, "peek back succeeds");
    TEST_ASSERT(memcmp(first, readback, 16) == 0,
                "child memory unchanged - the refused poke wrote nothing");

    MemTerminate(sr.token);
    MemReleaseAll();
}

int main(void)
{
    printf("Running mem_ops.c tests...\n\n");

    RUN_TEST(mem_parse_u32_accepts);
    RUN_TEST(mem_parse_u32_rejects);
    RUN_TEST(range_guard_ok);
    RUN_TEST(range_guard_wraparound);
    RUN_TEST(range_guard_cap);
    RUN_TEST(range_cap_default);
    RUN_TEST(region_accessible_decision);
    RUN_TEST(os_family_maps_to_tier);
    RUN_TEST(token_table_lifecycle);
    RUN_TEST(process_table_bounded);
    RUN_TEST(spawn_retain_requires_catalogued);
    RUN_TEST(peek_process_tier_round_trip);
    RUN_TEST(poke_requires_device_arm);
    RUN_TEST(poke_fail_closed_audit);

    print_test_summary();
    return g_tests_failed;
}
