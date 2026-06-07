/*
 * test_feat.c - Unit tests for feat.c (runtime feature detection)
 *
 * Obligations discharged (see tests/OBLIGATIONS-PHASE4.md):
 *   entity-fields.Capabilities    - has_* flag iff p* pointer non-NULL
 *   invariant.Win32sIsBaseline    - win32s probe => all uplift flags 0
 *   (PHASE4.md test_feat list #1-#6)
 *
 * Capability expectations hold on the dev host (Windows 11 via WSL
 * interop, which presents as NT): is_nt=1, has_threads=1.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "feat.h"

/* ========================================================
 * #1 FeatInit populates the struct without crashing, and
 *    reports the expected host capabilities (NT, threads).
 *    Obligation: entity-fields.Capabilities (PHASE4 feat #1, #2)
 * ======================================================== */

TEST_CASE(init_populates) {
    FeatInit();
    TEST_ASSERT_INT_EQUAL(1, g_features.is_nt, "host is NT (Win11/WSL interop)");
    TEST_ASSERT_INT_EQUAL(1, g_features.has_threads, "threads available on NT");
    TEST_ASSERT(g_features.win_major >= 4, "win_major >= 4 on NT");
}

/* ========================================================
 * #2 OS version decode is internally consistent: exactly
 *    one OS-kind flag dominates, and is_win32s is clear on
 *    a threaded NT host.
 *    Obligation: entity-fields.Capabilities (PHASE4 feat #2)
 * ======================================================== */

TEST_CASE(version_consistent) {
    FeatInit();
    TEST_ASSERT_INT_EQUAL(0, g_features.is_win32s, "not win32s on NT host");
    TEST_ASSERT_INT_EQUAL(0, g_features.is_win9x, "not 9x on NT host");
    /* NT carries a build number; 9x/win32s do not. */
    TEST_ASSERT(g_features.win_build > 0, "NT host has a build number");
}

/* ========================================================
 * #3 FeatVersionString is non-empty and starts with
 *    "Windows " (integer-only formatting).
 *    Obligation: entity-fields.Capabilities (PHASE4 feat #3)
 * ======================================================== */

TEST_CASE(version_string_format) {
    const char *s;
    FeatInit();
    s = FeatVersionString();
    TEST_ASSERT(s != NULL, "version string non-NULL");
    TEST_ASSERT(lstrlen(s) > 0, "version string non-empty");
    TEST_ASSERT(strncmp(s, "Windows ", 8) == 0, "starts with 'Windows '");
    /* NT host string ends with the "(NT)" kind tag. */
    TEST_ASSERT(strstr(s, "(NT)") != NULL, "NT host tagged (NT)");
}

/* ========================================================
 * #4 Each has_* flag is consistent with its function
 *    pointer(s): TRUE iff the backing pointer(s) are non-NULL.
 *    Obligation: entity-fields.Capabilities ("has_* iff p* non-NULL")
 * ======================================================== */

TEST_CASE(flags_match_pointers) {
    FeatInit();

    /* Single-pointer capabilities. */
    TEST_ASSERT_INT_EQUAL(g_features.pGetBinaryTypeA != NULL,
        g_features.has_get_binary_type, "get_binary_type flag iff ptr");
    TEST_ASSERT_INT_EQUAL(g_features.pIsWow64Process != NULL,
        g_features.has_is_wow64_process, "is_wow64_process flag iff ptr");
    TEST_ASSERT_INT_EQUAL(g_features.pGenerateConsoleCtrlEvent != NULL,
        g_features.has_generate_ctrl_event, "ctrl_event flag iff ptr");
    TEST_ASSERT_INT_EQUAL(g_features.pQueryFullProcessImageNameA != NULL,
        g_features.has_query_full_image_name, "query_full_image flag iff ptr");
    TEST_ASSERT_INT_EQUAL(g_features.pSetProcessMitigationPolicy != NULL,
        g_features.has_set_process_mitigation, "mitigation flag iff ptr");

    /* Multi-pointer capabilities: flag set => every backing ptr present. */
    if (g_features.has_create_job_object) {
        TEST_ASSERT(g_features.pCreateJobObjectA != NULL, "job: create ptr");
        TEST_ASSERT(g_features.pAssignProcessToJobObject != NULL, "job: assign ptr");
        TEST_ASSERT(g_features.pSetInformationJobObject != NULL, "job: setinfo ptr");
    }
    if (g_features.has_create_pseudo_console) {
        TEST_ASSERT(g_features.pCreatePseudoConsole != NULL, "pty: create ptr");
        TEST_ASSERT(g_features.pClosePseudoConsole != NULL, "pty: close ptr");
        TEST_ASSERT(g_features.pResizePseudoConsole != NULL, "pty: resize ptr");
    }
    if (g_features.has_proc_thread_attr_list) {
        TEST_ASSERT(g_features.pInitializeProcThreadAttributeList != NULL, "attr: init ptr");
        TEST_ASSERT(g_features.pUpdateProcThreadAttribute != NULL, "attr: update ptr");
        TEST_ASSERT(g_features.pDeleteProcThreadAttributeList != NULL, "attr: delete ptr");
    }
}

/* ========================================================
 * #5 has_create_pseudo_console reflects the host: when set,
 *    the PTY pointers are usable; when clear, they are NULL
 *    (skip-if-absent guard pattern for callers).
 *    Obligation: entity-fields.Capabilities (PHASE4 feat #5)
 * ======================================================== */

TEST_CASE(pseudo_console_reflects_host) {
    FeatInit();
    if (g_features.has_create_pseudo_console) {
        TEST_ASSERT(g_features.pCreatePseudoConsole != NULL,
            "pty present => create ptr usable");
    } else {
        TEST_ASSERT(g_features.pCreatePseudoConsole == NULL,
            "pty absent => create ptr NULL (callers skip)");
    }
}

/* ========================================================
 * #6 FeatForceFallback zeroes the selected flags AND their
 *    function pointers, so fallback paths can be exercised
 *    on uplifted hosts. Verifies flags and pointers both go
 *    to zero, and untouched capabilities are preserved.
 *    Obligation: invariant.Win32sIsBaseline (PHASE4 feat #6)
 * ======================================================== */

TEST_CASE(force_fallback_zeroes_flags_and_pointers) {
    int mask;
    int binary_before;
    BOOL (WINAPI *binary_ptr_before)(LPCSTR, LPDWORD);

    FeatInit();

    /* Capture an unrelated capability to confirm it is preserved. */
    binary_before = g_features.has_get_binary_type;
    binary_ptr_before = g_features.pGetBinaryTypeA;

    mask = FeatForceFallback(FEAT_FORCE_NO_THREADS |
                             FEAT_FORCE_NO_JOB_OBJECTS |
                             FEAT_FORCE_NO_CTRL_EVENTS);
    TEST_ASSERT_INT_EQUAL(FEAT_FORCE_NO_THREADS | FEAT_FORCE_NO_JOB_OBJECTS |
                          FEAT_FORCE_NO_CTRL_EVENTS, mask,
                          "returns the mask applied");

    /* Flags zeroed. */
    TEST_ASSERT_INT_EQUAL(0, g_features.has_threads, "threads flag zeroed");
    TEST_ASSERT_INT_EQUAL(0, g_features.has_create_job_object, "job flag zeroed");
    TEST_ASSERT_INT_EQUAL(0, g_features.has_generate_ctrl_event, "ctrl flag zeroed");

    /* Matching pointers zeroed. */
    TEST_ASSERT(g_features.pCreateJobObjectA == NULL, "job create ptr NULL");
    TEST_ASSERT(g_features.pAssignProcessToJobObject == NULL, "job assign ptr NULL");
    TEST_ASSERT(g_features.pSetInformationJobObject == NULL, "job setinfo ptr NULL");
    TEST_ASSERT(g_features.pGenerateConsoleCtrlEvent == NULL, "ctrl ptr NULL");

    /* Untouched capability preserved (mask did not include it). */
    TEST_ASSERT_INT_EQUAL(binary_before, g_features.has_get_binary_type,
        "unmasked binary_type flag preserved");
    TEST_ASSERT(g_features.pGetBinaryTypeA == binary_ptr_before,
        "unmasked binary_type ptr preserved");
}

/* ========================================================
 * #7 The 5.4 delay-loaded -W (UTF-16) uplift resolves on
 *    this NT host: all eight file/dir pointers plus
 *    CreateProcessW are non-NULL and the two flags are set.
 *    The has_wide_fileapi flag implies every backing pointer
 *    (the multi-pointer invariant).
 *    Obligation: entity-fields.Capabilities ("has_* iff p* non-NULL")
 * ======================================================== */

TEST_CASE(wide_uplift_probe) {
    FeatInit();

    /* File/dir tier: flag set on NT, every backing pointer present. */
    TEST_ASSERT_INT_EQUAL(1, g_features.has_wide_fileapi,
        "wide fileapi available on NT host");
    TEST_ASSERT(g_features.pCreateFileW != NULL, "wide: CreateFileW ptr");
    TEST_ASSERT(g_features.pFindFirstFileW != NULL, "wide: FindFirstFileW ptr");
    TEST_ASSERT(g_features.pFindNextFileW != NULL, "wide: FindNextFileW ptr");
    TEST_ASSERT(g_features.pDeleteFileW != NULL, "wide: DeleteFileW ptr");
    TEST_ASSERT(g_features.pCopyFileW != NULL, "wide: CopyFileW ptr");
    TEST_ASSERT(g_features.pMoveFileW != NULL, "wide: MoveFileW ptr");
    TEST_ASSERT(g_features.pCreateDirectoryW != NULL, "wide: CreateDirectoryW ptr");
    TEST_ASSERT(g_features.pRemoveDirectoryW != NULL, "wide: RemoveDirectoryW ptr");

    /* Spawn tier tracked separately. */
    TEST_ASSERT_INT_EQUAL(1, g_features.has_wide_createprocess,
        "wide createprocess available on NT host");
    TEST_ASSERT(g_features.pCreateProcessW != NULL, "wide: CreateProcessW ptr");

    /* Multi-pointer invariant: flag set => every backing pointer present. */
    if (g_features.has_wide_fileapi) {
        TEST_ASSERT(g_features.pCreateFileW != NULL &&
                    g_features.pFindFirstFileW != NULL &&
                    g_features.pFindNextFileW != NULL &&
                    g_features.pDeleteFileW != NULL &&
                    g_features.pCopyFileW != NULL &&
                    g_features.pMoveFileW != NULL &&
                    g_features.pCreateDirectoryW != NULL &&
                    g_features.pRemoveDirectoryW != NULL,
                    "has_wide_fileapi => all eight file/dir ptrs set");
    }
}

/* ========================================================
 * #8 FeatForceFallback drops the 5.4 -W uplift: both wide
 *    flags clear and ALL NINE -W pointers go NULL, so the
 *    codepage (-A) tier can be exercised on an NT host.
 *    An unrelated capability is preserved.
 *    Obligation: invariant.Win32sIsBaseline
 * ======================================================== */

TEST_CASE(force_fallback_wide) {
    int mask;
    int binary_before;
    BOOL (WINAPI *binary_ptr_before)(LPCSTR, LPDWORD);

    FeatInit();

    /* Capture an unrelated capability to confirm it is preserved. */
    binary_before = g_features.has_get_binary_type;
    binary_ptr_before = g_features.pGetBinaryTypeA;

    mask = FeatForceFallback(FEAT_FORCE_NO_WIDE_FILEAPI |
                             FEAT_FORCE_NO_WIDE_CREATEPROCESS);
    TEST_ASSERT_INT_EQUAL(FEAT_FORCE_NO_WIDE_FILEAPI |
                          FEAT_FORCE_NO_WIDE_CREATEPROCESS, mask,
                          "returns the mask applied");

    /* Flags zeroed. */
    TEST_ASSERT_INT_EQUAL(0, g_features.has_wide_fileapi, "wide fileapi flag zeroed");
    TEST_ASSERT_INT_EQUAL(0, g_features.has_wide_createprocess,
        "wide createprocess flag zeroed");

    /* All nine -W pointers zeroed. */
    TEST_ASSERT(g_features.pCreateFileW == NULL, "CreateFileW ptr NULL");
    TEST_ASSERT(g_features.pFindFirstFileW == NULL, "FindFirstFileW ptr NULL");
    TEST_ASSERT(g_features.pFindNextFileW == NULL, "FindNextFileW ptr NULL");
    TEST_ASSERT(g_features.pDeleteFileW == NULL, "DeleteFileW ptr NULL");
    TEST_ASSERT(g_features.pCopyFileW == NULL, "CopyFileW ptr NULL");
    TEST_ASSERT(g_features.pMoveFileW == NULL, "MoveFileW ptr NULL");
    TEST_ASSERT(g_features.pCreateDirectoryW == NULL, "CreateDirectoryW ptr NULL");
    TEST_ASSERT(g_features.pRemoveDirectoryW == NULL, "RemoveDirectoryW ptr NULL");
    TEST_ASSERT(g_features.pCreateProcessW == NULL, "CreateProcessW ptr NULL");

    /* Untouched capability preserved (mask did not include it). */
    TEST_ASSERT_INT_EQUAL(binary_before, g_features.has_get_binary_type,
        "unmasked binary_type flag preserved");
    TEST_ASSERT(g_features.pGetBinaryTypeA == binary_ptr_before,
        "unmasked binary_type ptr preserved");
}

/* The os_family_maps_to_tier / EncTierCurrent test is added with the
 * encoding tier half (a later 5.4 module supplies that function). */

int main(void)
{
    RUN_TEST(init_populates);
    RUN_TEST(version_consistent);
    RUN_TEST(version_string_format);
    RUN_TEST(flags_match_pointers);
    RUN_TEST(pseudo_console_reflects_host);
    RUN_TEST(force_fallback_zeroes_flags_and_pointers);
    RUN_TEST(wide_uplift_probe);
    RUN_TEST(force_fallback_wide);

    print_test_summary();
    return g_tests_failed;
}
