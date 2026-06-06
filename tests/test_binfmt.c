/*
 * test_binfmt.c - Unit tests for the MZ/NE/PE binary-format classifier.
 *
 * Covers the 6 PHASE4.md tests (plan/PHASE4.md "tests/test_binfmt.c (6)")
 * plus one uplift test (g_features.pGetBinaryTypeA path), tracing to the
 * binfmt obligations in tests/OBLIGATIONS-PHASE4.md.
 *
 * The fixtures (tiny_mz.exe / tiny_ne.exe) are located relative to this
 * test executable's own directory via GetModuleFileNameA, so the test
 * works regardless of the CTest working directory; it falls back to
 * "fixtures/..." and "tests/fixtures/..." under the CWD.
 *
 * Build (standalone, without src/feat.c):
 *   i686-w64-mingw32-gcc -std=c89 -march=i386 -mtune=i386 -Wall -Werror \
 *     -pedantic -Wdouble-promotion -Wfloat-equal -Isrc \
 *     -DBINFMT_TEST_NO_FEAT \
 *     -o tests/tmp_test_binfmt.exe tests/test_binfmt.c src/binfmt.c
 *
 * Build (with the real feature module):
 *   ... -o tests/tmp_test_binfmt.exe tests/test_binfmt.c src/binfmt.c src/feat.c
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdio.h>
#include "test_framework.h"
#include "binfmt.h"
#include "feat.h"

#ifdef BINFMT_TEST_NO_FEAT
/* Local fallback so the classifier links without src/feat.c. A zeroed
   g_features forces the manual-header path in BinFmtClassify. */
Features g_features = {0};
void FeatInit(void) {}
#endif

/* Directory of this test executable (with trailing separator). */
static char g_exeDir[MAX_PATH];

/*
 * init_exe_dir - Fill g_exeDir from GetModuleFileNameA, trimming the
 * file name to leave the directory plus a trailing backslash.
 */
static void init_exe_dir(void)
{
    DWORD len;
    int i;
    int lastSep;

    g_exeDir[0] = '\0';
    len = GetModuleFileNameA(NULL, g_exeDir, (DWORD)MAX_PATH);
    if (len == 0 || len >= (DWORD)MAX_PATH) {
        g_exeDir[0] = '\0';
        return;
    }
    lastSep = -1;
    for (i = 0; g_exeDir[i] != '\0'; i++) {
        if (g_exeDir[i] == '\\' || g_exeDir[i] == '/') {
            lastSep = i;
        }
    }
    if (lastSep < 0) {
        g_exeDir[0] = '\0';
    } else {
        g_exeDir[lastSep + 1] = '\0';
    }
}

/*
 * file_exists - True if a regular file exists at path.
 */
static int file_exists(const char *path)
{
    DWORD attrs;
    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
}

/*
 * fixture_path - Resolve a fixture file name to a usable path, trying
 * (in order) the test-exe directory, "fixtures/", and "tests/fixtures/".
 * Returns 1 and fills out on success, 0 if not found.
 */
static int fixture_path(const char *name, char *out, int outSize)
{
    /* exe-dir + "fixtures\" + name */
    if (g_exeDir[0] != '\0') {
        _snprintf(out, outSize, "%sfixtures\\%s", g_exeDir, name);
        out[outSize - 1] = '\0';
        if (file_exists(out)) {
            return 1;
        }
        _snprintf(out, outSize, "%s%s", g_exeDir, name);
        out[outSize - 1] = '\0';
        if (file_exists(out)) {
            return 1;
        }
    }
    _snprintf(out, outSize, "fixtures\\%s", name);
    out[outSize - 1] = '\0';
    if (file_exists(out)) {
        return 1;
    }
    _snprintf(out, outSize, "tests\\fixtures\\%s", name);
    out[outSize - 1] = '\0';
    if (file_exists(out)) {
        return 1;
    }
    return 0;
}

/* 1. mcp-w32s.exe (here: this test exe itself, a real PE32) -> BIN_PE32 */
TEST_CASE(self_is_pe32) {
    char path[MAX_PATH];
    char err[128];
    BinaryType type;
    DWORD len;
    int ok;

    len = GetModuleFileNameA(NULL, path, (DWORD)MAX_PATH);
    TEST_ASSERT(len > 0 && len < (DWORD)MAX_PATH, "got module file name");

    type = BIN_UNKNOWN;
    ok = BinFmtClassify(path, &type, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "classify self succeeds");
    TEST_ASSERT_INT_EQUAL(BIN_PE32, type, "self is PE32");
}

/* 2. Fixture NE16 -> BIN_NE16 */
TEST_CASE(fixture_ne_is_ne16) {
    char path[MAX_PATH];
    char err[128];
    BinaryType type;
    int ok;

    TEST_ASSERT(fixture_path("tiny_ne.exe", path, sizeof(path)),
                "located tiny_ne.exe");
    type = BIN_UNKNOWN;
    ok = BinFmtClassify(path, &type, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "classify NE fixture succeeds");
    TEST_ASSERT_INT_EQUAL(BIN_NE16, type, "NE fixture is NE16");
}

/* 3. Fixture MZ -> BIN_MZ */
TEST_CASE(fixture_mz_is_mz) {
    char path[MAX_PATH];
    char err[128];
    BinaryType type;
    int ok;

    TEST_ASSERT(fixture_path("tiny_mz.exe", path, sizeof(path)),
                "located tiny_mz.exe");
    type = BIN_UNKNOWN;
    ok = BinFmtClassify(path, &type, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "classify MZ fixture succeeds");
    TEST_ASSERT_INT_EQUAL(BIN_MZ, type, "MZ fixture is MZ");
}

/* 4. Text file (temp) -> BIN_UNKNOWN */
TEST_CASE(text_file_is_unknown) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    char err[128];
    BinaryType type;
    DWORD wrote;
    HANDLE hFile;
    int ok;
    const char *text = "this is a plain text file, not an executable\r\n";

    GetTempPathA((DWORD)MAX_PATH, dir);
    _snprintf(path, sizeof(path), "%sbinfmt_text.txt", dir);
    path[sizeof(path) - 1] = '\0';

    hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_ASSERT(hFile != INVALID_HANDLE_VALUE, "created temp text file");
    WriteFile(hFile, text, (DWORD)strlen(text), &wrote, NULL);
    CloseHandle(hFile);

    type = BIN_PE32;
    ok = BinFmtClassify(path, &type, err, sizeof(err));
    DeleteFileA(path);
    TEST_ASSERT_INT_EQUAL(1, ok, "classify text file succeeds");
    TEST_ASSERT_INT_EQUAL(BIN_UNKNOWN, type, "text file is unknown");
}

/* 5. Missing file with explicit path -> error return (0) */
TEST_CASE(missing_file_errors) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    char err[128];
    BinaryType type;
    int ok;

    GetTempPathA((DWORD)MAX_PATH, dir);
    _snprintf(path, sizeof(path), "%sbinfmt_no_such_file.exe", dir);
    path[sizeof(path) - 1] = '\0';
    DeleteFileA(path);  /* ensure absent */

    err[0] = '\0';
    type = BIN_PE32;
    ok = BinFmtClassify(path, &type, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "classify missing file fails");
    TEST_ASSERT(err[0] != '\0', "error message populated");
}

/* 6. Shell built-in name ("dir") -> BIN_SHELL without file read */
TEST_CASE(shell_builtin_is_shell) {
    char err[128];
    BinaryType type;
    int ok;

    type = BIN_UNKNOWN;
    ok = BinFmtClassify("dir", &type, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "classify 'dir' succeeds");
    TEST_ASSERT_INT_EQUAL(BIN_SHELL, type, "'dir' is shell built-in");
}

/* 7. Uplift: after FeatInit, if pGetBinaryTypeA present, self still PE32. */
TEST_CASE(uplift_self_pe32) {
    char path[MAX_PATH];
    char err[128];
    BinaryType type;
    int ok;

    FeatInit();
#ifndef BINFMT_TEST_NO_FEAT
    if (g_features.pGetBinaryTypeA == 0) {
        /* No uplift available on this host; the manual path is covered
           by self_is_pe32. Nothing to assert here. */
        return;
    }
#endif
    GetModuleFileNameA(NULL, path, (DWORD)MAX_PATH);
    type = BIN_UNKNOWN;
    ok = BinFmtClassify(path, &type, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "uplift classify self succeeds");
    TEST_ASSERT_INT_EQUAL(BIN_PE32, type, "uplift: self still PE32");
}

int main(void)
{
    init_exe_dir();
    printf("  Exe dir: %s\n\n", g_exeDir);

    RUN_TEST(self_is_pe32);
    RUN_TEST(fixture_ne_is_ne16);
    RUN_TEST(fixture_mz_is_mz);
    RUN_TEST(text_file_is_unknown);
    RUN_TEST(missing_file_errors);
    RUN_TEST(shell_builtin_is_shell);
    RUN_TEST(uplift_self_pe32);

    print_test_summary();
    return g_tests_failed;
}
