/*
 * test_framework.h - Minimal C89 test framework for MCP-Win32s
 *
 * Header-only. No external dependencies. ~100 lines.
 * Compiles with VC++ 6.0, MinGW, and Visual Studio 2022.
 * Runs on Windows 3.1 + Win32s 1.25a through Windows 11.
 * Runs under Wine on Linux for CI.
 *
 * Usage:
 *   #include "test_framework.h"
 *
 *   TEST_CASE(my_test) {
 *       TEST_ASSERT(1 == 1, "math works");
 *       TEST_ASSERT_STR_EQUAL("hello", "hello", "strings match");
 *   }
 *
 *   int main(void) {
 *       RUN_TEST(my_test);
 *       print_test_summary();
 *       return g_tests_failed;
 *   }
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>

/* Global test counters */
static int g_tests_run = 0;
static int g_tests_failed = 0;
static const char *g_current_test = "";

/*
 * TEST_ASSERT - Assert a condition. On failure, prints message and returns.
 */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("  FAIL: %s - %s\n", g_current_test, (message)); \
            g_tests_failed++; \
            return; \
        } \
    } while (0)

/*
 * TEST_ASSERT_INT_EQUAL - Assert two integers are equal.
 */
#define TEST_ASSERT_INT_EQUAL(expected, actual, message) \
    do { \
        int _exp = (expected); \
        int _act = (actual); \
        if (_exp != _act) { \
            printf("  FAIL: %s - %s (expected %d, got %d)\n", \
                   g_current_test, (message), _exp, _act); \
            g_tests_failed++; \
            return; \
        } \
    } while (0)

/*
 * TEST_ASSERT_STR_EQUAL - Assert two strings are equal.
 */
#define TEST_ASSERT_STR_EQUAL(expected, actual, message) \
    do { \
        const char *_exp = (expected); \
        const char *_act = (actual); \
        if (strcmp(_exp, _act) != 0) { \
            printf("  FAIL: %s - %s\n    expected: \"%s\"\n    actual:   \"%s\"\n", \
                   g_current_test, (message), _exp, _act); \
            g_tests_failed++; \
            return; \
        } \
    } while (0)

/*
 * TEST_CASE - Declare a test case function.
 * Creates both the test function and a runner wrapper.
 */
#define TEST_CASE(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        g_current_test = #name; \
        g_tests_run++; \
        test_##name(); \
    } \
    static void test_##name(void)

/*
 * RUN_TEST - Execute a test case and print its name.
 * Tracks failure count to detect if the test failed.
 */
#define RUN_TEST(name) \
    do { \
        int _before = g_tests_failed; \
        printf("  %s ... ", #name); \
        run_##name(); \
        if (g_tests_failed == _before) { \
            printf("ok\n"); \
        } \
    } while (0)

/*
 * print_test_summary - Print final pass/fail counts.
 */
static void print_test_summary(void)
{
    printf("\n========================================\n");
    printf("Tests run:    %d\n", g_tests_run);
    printf("Tests failed: %d\n", g_tests_failed);
    printf("Tests passed: %d\n", g_tests_run - g_tests_failed);
    printf("========================================\n");

    if (g_tests_failed == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("SOME TESTS FAILED\n");
    }
}

#endif /* TEST_FRAMEWORK_H */
