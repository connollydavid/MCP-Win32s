/*
 * test_toolchain_probe.c - Unit tests for toolchain_probe.c
 *
 * Obligations discharged (see tests/OBLIGATIONS-5.2.md, Device: toolchain
 * detection):
 *   rule-success.ToolchainDetected,
 *   rule-entity-creation.ToolchainDetected.1,
 *   entity-fields.DetectedToolchain                  - banner match + fields
 *   entity-fields.DetectedToolchain (full-version)   - full build number kept
 *   (ready-array shape; wire-contract.allium ReadyShape) - ToolchainAppendJson
 *
 * ToolchainMatchBanner and ToolchainAppendJson are pure string logic and run
 * everywhere. ToolchainProbe with a NULL catalog needs no toolchain installed;
 * real-compiler detection is host-dependent and not hard-required here (the
 * bridge-side integration tests exercise the populated path).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "toolchain_probe.h"

/*
 * copy_field - test helper: copy a literal into a fixed field.
 */
static void copy_field(char *dst, const char *src)
{
    int i;
    for (i = 0; src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ========================================================
 * ToolchainMatchBanner - a real MSVC banner is recognised: vendor
 * "Microsoft", command echoed back, version is the FULL build number
 * "12.00.8804" (the SP-distinguishing field; docs/build-toolchain-flags.md).
 *   Obligations: rule-success.ToolchainDetected,
 *     rule-entity-creation.ToolchainDetected.1,
 *     entity-fields.DetectedToolchain
 * ======================================================== */

TEST_CASE(match_msvc_banner) {
    DetectedToolchain det;
    const char *banner =
        "Microsoft (R) 32-bit C/C++ Optimizing Compiler "
        "Version 12.00.8804 for 80x86\r\n"
        "Copyright (C) Microsoft Corp 1984-1998. All rights reserved.\r\n";
    int ok;

    memset(&det, 0, sizeof(det));
    ok = ToolchainMatchBanner("cl", banner, &det);
    TEST_ASSERT_INT_EQUAL(1, ok, "MSVC banner recognised");
    TEST_ASSERT_STR_EQUAL("Microsoft", det.vendor, "vendor is Microsoft");
    TEST_ASSERT_STR_EQUAL("cl", det.command, "command echoed back");
    TEST_ASSERT_STR_EQUAL("12.00.8804", det.version,
                          "full build number retained");
}

/* ========================================================
 * ToolchainMatchBanner full-version retention: a different VC6 build
 * (8168, the RTM/SP1-SP4 banner) is kept in full, not truncated to "12".
 *   Obligation: entity-fields.DetectedToolchain (full-version retention)
 * ======================================================== */

TEST_CASE(match_msvc_full_version_not_truncated) {
    DetectedToolchain det;
    const char *banner =
        "Microsoft (R) 32-bit C/C++ Optimizing Compiler "
        "Version 12.00.8168 for 80x86\r\n";
    int ok;

    memset(&det, 0, sizeof(det));
    ok = ToolchainMatchBanner("cl", banner, &det);
    TEST_ASSERT_INT_EQUAL(1, ok, "MSVC 8168 banner recognised");
    TEST_ASSERT_STR_EQUAL("12.00.8168", det.version,
                          "build number 8168 kept in full");
}

/* ========================================================
 * ToolchainMatchBanner - a real Open Watcom banner (wcc386) is
 * recognised: vendor "Open Watcom", version is the token after "Version ".
 *   Obligations: rule-success.ToolchainDetected,
 *     entity-fields.DetectedToolchain
 * ======================================================== */

TEST_CASE(match_watcom_banner) {
    DetectedToolchain det;
    const char *banner =
        "Open Watcom C32 Optimizing Compiler Version 2.0 beta\r\n"
        "Portions Copyright (c) 1984-2002 Sybase, Inc. "
        "All Rights Reserved.\r\n";
    int ok;

    memset(&det, 0, sizeof(det));
    ok = ToolchainMatchBanner("wcc386", banner, &det);
    TEST_ASSERT_INT_EQUAL(1, ok, "Watcom banner recognised");
    TEST_ASSERT_STR_EQUAL("Open Watcom", det.vendor, "vendor is Open Watcom");
    TEST_ASSERT_STR_EQUAL("wcc386", det.command, "command echoed back");
    TEST_ASSERT_STR_EQUAL("2.0", det.version, "version token extracted");
}

/* ========================================================
 * ToolchainMatchBanner - the wcl386 driver shares the Watcom vendor and
 * the "Version " anchor (it is a separate table entry).
 * ======================================================== */

TEST_CASE(match_watcom_driver) {
    DetectedToolchain det;
    const char *banner =
        "Open Watcom C/C++ x86 32-bit Compile and Link Utility "
        "Version 1.9\r\n";
    int ok;

    memset(&det, 0, sizeof(det));
    ok = ToolchainMatchBanner("wcl386", banner, &det);
    TEST_ASSERT_INT_EQUAL(1, ok, "Watcom driver banner recognised");
    TEST_ASSERT_STR_EQUAL("Open Watcom", det.vendor, "vendor is Open Watcom");
    TEST_ASSERT_STR_EQUAL("1.9", det.version, "version token extracted");
}

/* ========================================================
 * ToolchainMatchBanner negatives: an unknown command never matches even
 * with a versioned banner; a known command with no anchor in the banner
 * does not match either.
 *   Obligations: rule-failure (ToolchainDetected does not fire)
 * ======================================================== */

TEST_CASE(match_unknown_command_rejected) {
    DetectedToolchain det;
    const char *banner =
        "GNU C Compiler Version 9.3.0\r\n";
    int ok;

    memset(&det, 0, sizeof(det));
    ok = ToolchainMatchBanner("gcc", banner, &det);
    TEST_ASSERT_INT_EQUAL(0, ok, "unknown command not detected");
}

TEST_CASE(match_anchorless_banner_rejected) {
    DetectedToolchain det;
    const char *banner =
        "Microsoft (R) 32-bit C/C++ Optimizing Compiler\r\n"
        "no recognised version field here\r\n";
    int ok;

    memset(&det, 0, sizeof(det));
    ok = ToolchainMatchBanner("cl", banner, &det);
    TEST_ASSERT_INT_EQUAL(0, ok, "anchorless banner not detected");
}

/* ========================================================
 * ToolchainAppendJson empty set -> exactly "toolchains":[].
 *   Obligation: ready-array shape / ReadyShape (empty array, not absent)
 * ======================================================== */

TEST_CASE(append_json_empty) {
    ToolchainSet set;
    char json[256];
    int pos;
    int ok;

    set.count = 0;
    json[0] = '\0';
    pos = 0;
    ok = ToolchainAppendJson(&set, json, (int)sizeof(json), &pos);
    TEST_ASSERT_INT_EQUAL(1, ok, "append succeeds");
    TEST_ASSERT_STR_EQUAL("\"toolchains\":[]", json, "empty array form");
    TEST_ASSERT_INT_EQUAL(15, pos, "pos advanced by length of empty form");
}

/* ========================================================
 * ToolchainAppendJson one entry -> exact object JSON.
 *   Obligation: ready-array shape (one {vendor,command,version})
 * ======================================================== */

TEST_CASE(append_json_one_entry) {
    ToolchainSet set;
    char json[256];
    int pos;
    int ok;

    set.count = 1;
    copy_field(set.items[0].vendor, "Microsoft");
    copy_field(set.items[0].command, "cl");
    copy_field(set.items[0].version, "12.00.8804");

    json[0] = '\0';
    pos = 0;
    ok = ToolchainAppendJson(&set, json, (int)sizeof(json), &pos);
    TEST_ASSERT_INT_EQUAL(1, ok, "append succeeds");
    TEST_ASSERT_STR_EQUAL(
        "\"toolchains\":[{\"vendor\":\"Microsoft\",\"command\":\"cl\","
        "\"version\":\"12.00.8804\"}]",
        json, "one-entry JSON exact");
}

/* ========================================================
 * ToolchainAppendJson two entries -> comma-separated.
 * ======================================================== */

TEST_CASE(append_json_two_entries) {
    ToolchainSet set;
    char json[512];
    int pos;
    int ok;

    set.count = 2;
    copy_field(set.items[0].vendor, "Microsoft");
    copy_field(set.items[0].command, "cl");
    copy_field(set.items[0].version, "12.00.8804");
    copy_field(set.items[1].vendor, "Open Watcom");
    copy_field(set.items[1].command, "wcc386");
    copy_field(set.items[1].version, "2.0");

    json[0] = '\0';
    pos = 0;
    ok = ToolchainAppendJson(&set, json, (int)sizeof(json), &pos);
    TEST_ASSERT_INT_EQUAL(1, ok, "append succeeds");
    TEST_ASSERT_STR_EQUAL(
        "\"toolchains\":[{\"vendor\":\"Microsoft\",\"command\":\"cl\","
        "\"version\":\"12.00.8804\"},"
        "{\"vendor\":\"Open Watcom\",\"command\":\"wcc386\","
        "\"version\":\"2.0\"}]",
        json, "two-entry comma-separated JSON exact");
}

/* ========================================================
 * ToolchainAppendJson escapes a value containing a double quote.
 * ======================================================== */

TEST_CASE(append_json_escapes_quote) {
    ToolchainSet set;
    char json[256];
    int pos;
    int ok;

    set.count = 1;
    copy_field(set.items[0].vendor, "Acme \"X\"");
    copy_field(set.items[0].command, "cc");
    copy_field(set.items[0].version, "1.0");

    json[0] = '\0';
    pos = 0;
    ok = ToolchainAppendJson(&set, json, (int)sizeof(json), &pos);
    TEST_ASSERT_INT_EQUAL(1, ok, "append succeeds");
    TEST_ASSERT_STR_EQUAL(
        "\"toolchains\":[{\"vendor\":\"Acme \\\"X\\\"\",\"command\":\"cc\","
        "\"version\":\"1.0\"}]",
        json, "embedded quote escaped");
}

/* ========================================================
 * ToolchainAppendJson into a too-small buffer returns 0 (overflow).
 * ======================================================== */

TEST_CASE(append_json_overflow) {
    ToolchainSet set;
    char json[16];
    int pos;
    int ok;

    set.count = 1;
    copy_field(set.items[0].vendor, "Microsoft");
    copy_field(set.items[0].command, "cl");
    copy_field(set.items[0].version, "12.00.8804");

    json[0] = '\0';
    pos = 0;
    ok = ToolchainAppendJson(&set, json, (int)sizeof(json), &pos);
    TEST_ASSERT_INT_EQUAL(0, ok, "overflow returns 0");
}

/* ========================================================
 * ToolchainProbe with cat == NULL -> 0, count 0 (detects nothing).
 *   Obligation: ToolchainProbe defensive contract (no catalogue -> none)
 * ======================================================== */

TEST_CASE(probe_null_catalog) {
    ToolchainSet set;
    int n;

    set.count = 99;
    n = ToolchainProbe(NULL, &set);
    TEST_ASSERT_INT_EQUAL(0, n, "NULL catalog detects nothing");
    TEST_ASSERT_INT_EQUAL(0, set.count, "count reset to 0");
}

int main(void)
{
    printf("Running toolchain_probe tests...\n");
    RUN_TEST(match_msvc_banner);
    RUN_TEST(match_msvc_full_version_not_truncated);
    RUN_TEST(match_watcom_banner);
    RUN_TEST(match_watcom_driver);
    RUN_TEST(match_unknown_command_rejected);
    RUN_TEST(match_anchorless_banner_rejected);
    RUN_TEST(append_json_empty);
    RUN_TEST(append_json_one_entry);
    RUN_TEST(append_json_two_entries);
    RUN_TEST(append_json_escapes_quote);
    RUN_TEST(append_json_overflow);
    RUN_TEST(probe_null_catalog);
    print_test_summary();
    return g_tests_failed;
}
