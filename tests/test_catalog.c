/*
 * test_catalog.c - Unit tests for catalog.c (Phase 4)
 *
 * Obligations (tests/OBLIGATIONS-PHASE4.md, specs/catalog.allium):
 *   enum-comparable.EntryKind ............... lookup_dir_is_builtin
 *   entity-fields.Catalog ................... load_count_at_least_30,
 *                                             invariant.LoadedCatalogHasEntries
 *   entity-fields.CatalogEntry .............. lookup_dir_is_builtin
 *   rule-success.CatalogLoadRecorded ........ load_count_at_least_30,
 *                                             missing_file_errors,
 *                                             malformed_json_errors
 *   rule-success.GateBuiltinHit ............. validate_dir_b_ok,
 *                                             builtin_route_data_available
 *   rule-success.GateExternalHit ............ validate_cl_tc_ok
 *   rule-success.GateMiss ................... lookup_unknown_is_null
 *   rule-success.GateArgsInvalid ............ validate_dir_unknown_rejected
 *   gate exclusivity / glued vs split ....... validate_glued_equals_split
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "catalog.h"

/* Resolved catalog path, found once in main. */
static char g_catalogPath[MAX_PATH];

/*
 * fileExists - True if a regular file is present at path.
 */
static int fileExists(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/*
 * findCatalog - Locate catalog/win32-commands.json. Tries the cwd-relative
 * paths first, then relative to the executable directory.
 */
static int findCatalog(char *out, int outSize)
{
    static const char *rel[] = {
        "catalog/win32-commands.json",
        "../catalog/win32-commands.json"
    };
    int i;
    char exeDir[MAX_PATH];
    DWORD n;

    for (i = 0; i < 2; i++) {
        if (fileExists(rel[i])) {
            lstrcpynA(out, rel[i], outSize);
            return 1;
        }
    }

    n = GetModuleFileNameA(NULL, exeDir, sizeof(exeDir));
    if (n > 0 && n < sizeof(exeDir)) {
        /* Strip the filename to leave the directory. */
        char *slash = exeDir;
        char *last = NULL;
        while (*slash != '\0') {
            if (*slash == '\\' || *slash == '/') {
                last = slash;
            }
            slash++;
        }
        if (last != NULL) {
            *(last + 1) = '\0';
            {
                char candidate[MAX_PATH];
                /* exeDir + "catalog/win32-commands.json" */
                lstrcpynA(candidate, exeDir, sizeof(candidate));
                lstrcatA(candidate, "catalog\\win32-commands.json");
                if (fileExists(candidate)) {
                    lstrcpynA(out, candidate, outSize);
                    return 1;
                }
                /* exeDir + "../catalog/win32-commands.json" */
                lstrcpynA(candidate, exeDir, sizeof(candidate));
                lstrcatA(candidate, "..\\catalog\\win32-commands.json");
                if (fileExists(candidate)) {
                    lstrcpynA(out, candidate, outSize);
                    return 1;
                }
            }
        }
    }
    return 0;
}

TEST_CASE(load_count_at_least_30) {
    Catalog *cat;
    char err[128];
    int ok;

    ok = CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "load succeeds");
    TEST_ASSERT(CatalogCount(cat) >= 30, "at least 30 entries");
    /* invariant.LoadedCatalogHasEntries: loaded implies entry_count > 0. */
    TEST_ASSERT(CatalogCount(cat) > 0, "loaded catalog has entries");
    CatalogFree(cat);
}

TEST_CASE(missing_file_errors) {
    Catalog *cat;
    char err[128];
    int ok;

    err[0] = '\0';
    cat = (Catalog *)1;
    ok = CatalogLoad("no_such_catalog_xyz.json", &cat, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "missing file returns 0");
    TEST_ASSERT_INT_EQUAL(0, (int)(err[0] == '\0'), "err message non-empty");
    TEST_ASSERT(cat == NULL, "out pointer cleared on failure");
}

TEST_CASE(malformed_json_errors) {
    Catalog *cat;
    char err[128];
    char tmpDir[MAX_PATH];
    char path[MAX_PATH];
    int ok;
    HANDLE hFile;
    DWORD written;
    const char *bad = "{ \"version\": 1, \"commands\": { \"dir\": { ";

    GetTempPathA(sizeof(tmpDir), tmpDir);
    lstrcpynA(path, tmpDir, sizeof(path));
    lstrcatA(path, "mcp_bad_catalog.json");

    hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_ASSERT(hFile != INVALID_HANDLE_VALUE, "temp file created");
    WriteFile(hFile, bad, (DWORD)lstrlenA(bad), &written, NULL);
    CloseHandle(hFile);

    err[0] = '\0';
    ok = CatalogLoad(path, &cat, err, sizeof(err));
    DeleteFileA(path);
    TEST_ASSERT_INT_EQUAL(0, ok, "malformed JSON returns 0");
    TEST_ASSERT_INT_EQUAL(0, (int)(err[0] == '\0'), "err message non-empty");
}

TEST_CASE(lookup_dir_is_builtin) {
    Catalog *cat;
    char err[128];
    const CatalogEntry *e;

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    e = CatalogLookup(cat, "dir");
    TEST_ASSERT(e != NULL, "dir found");
    TEST_ASSERT_INT_EQUAL(1, CatalogEntryIsBuiltin(e), "dir is shell-builtin");
    /* Case-insensitive lookup. */
    TEST_ASSERT(CatalogLookup(cat, "DIR") != NULL, "DIR found case-insensitive");
    CatalogFree(cat);
}

TEST_CASE(lookup_unknown_is_null) {
    Catalog *cat;
    char err[128];

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    TEST_ASSERT(CatalogLookup(cat, "unknown_xyz") == NULL,
                "unknown command returns NULL");
    CatalogFree(cat);
}

TEST_CASE(validate_dir_b_ok) {
    Catalog *cat;
    char err[128];
    const CatalogEntry *e;
    const char *argv[2];
    int ok;

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    e = CatalogLookup(cat, "dir");
    argv[0] = "dir";
    argv[1] = "/B";
    err[0] = '\0';
    ok = CatalogValidateArgs(e, argv, 2, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "dir /B is allowed");
    CatalogFree(cat);
}

TEST_CASE(validate_dir_unknown_rejected) {
    Catalog *cat;
    char err[128];
    const CatalogEntry *e;
    const char *argv[2];
    int ok;

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    e = CatalogLookup(cat, "dir");
    argv[0] = "dir";
    argv[1] = "/UNKNOWN";
    err[0] = '\0';
    ok = CatalogValidateArgs(e, argv, 2, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "dir /UNKNOWN rejected");
    TEST_ASSERT_STR_EQUAL("argument not allowed", err,
                          "reports argument not allowed");
    CatalogFree(cat);
}

TEST_CASE(validate_cl_tc_ok) {
    Catalog *cat;
    char err[128];
    const CatalogEntry *e;
    const char *argv[3];
    int ok;

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    e = CatalogLookup(cat, "cl");
    TEST_ASSERT(e != NULL, "cl found");
    TEST_ASSERT_INT_EQUAL(0, CatalogEntryIsBuiltin(e), "cl is external");
    argv[0] = "cl";
    argv[1] = "/TC";
    argv[2] = "file.c";
    err[0] = '\0';
    ok = CatalogValidateArgs(e, argv, 3, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "cl /TC file.c is allowed");
    CatalogFree(cat);
}

/*
 * Obligation (catalog.allium): glued (/A:v) and split (/A v) flag-arg forms
 * validate identically. cl /D defines a macro and declares an arg.
 */
TEST_CASE(validate_glued_equals_split) {
    Catalog *cat;
    char err[128];
    const CatalogEntry *e;
    const char *split[3];
    const char *glued[2];
    int okSplit;
    int okGlued;

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    e = CatalogLookup(cat, "cl");

    /* Split: /D consumes "WIN32" as its argument. */
    split[0] = "cl";
    split[1] = "/D";
    split[2] = "WIN32";
    okSplit = CatalogValidateArgs(e, split, 3, err, sizeof(err));

    /* Glued: /DWIN32 is one token. */
    glued[0] = "cl";
    glued[1] = "/DWIN32";
    okGlued = CatalogValidateArgs(e, glued, 2, err, sizeof(err));

    TEST_ASSERT_INT_EQUAL(1, okSplit, "split /D WIN32 allowed");
    TEST_ASSERT_INT_EQUAL(1, okGlued, "glued /DWIN32 allowed");
    TEST_ASSERT_INT_EQUAL(okSplit, okGlued, "glued and split agree");
    CatalogFree(cat);
}

/*
 * Obligation (rule-entity-creation.GateBuiltinHit / decision 3): the data
 * the dispatcher needs to auto-route a built-in is exposed via accessors --
 * the era-correct shell strings and supports_win32s.
 */
TEST_CASE(builtin_route_data_available) {
    Catalog *cat;
    char err[128];
    const CatalogEntry *e;

    CatalogLoad(g_catalogPath, &cat, err, sizeof(err));
    e = CatalogLookup(cat, "dir");
    TEST_ASSERT(CatalogEntryShellModern(e) != NULL, "shell_modern available");
    TEST_ASSERT(CatalogEntryShellWin32s(e) != NULL, "shell_win32s available");
    TEST_ASSERT(lstrlenA(CatalogEntryShellModern(e)) > 0,
                "shell_modern non-empty");
    TEST_ASSERT(lstrlenA(CatalogEntryShellWin32s(e)) > 0,
                "shell_win32s non-empty");
    TEST_ASSERT_INT_EQUAL(1, CatalogEntrySupportsWin32s(e),
                          "dir supports win32s");
    CatalogFree(cat);
}

int main(void)
{
    if (!findCatalog(g_catalogPath, sizeof(g_catalogPath))) {
        printf("FATAL: could not locate catalog/win32-commands.json\n");
        return 1;
    }
    printf("  Catalog: %s\n\n", g_catalogPath);

    RUN_TEST(load_count_at_least_30);
    RUN_TEST(missing_file_errors);
    RUN_TEST(malformed_json_errors);
    RUN_TEST(lookup_dir_is_builtin);
    RUN_TEST(lookup_unknown_is_null);
    RUN_TEST(validate_dir_b_ok);
    RUN_TEST(validate_dir_unknown_rejected);
    RUN_TEST(validate_cl_tc_ok);
    RUN_TEST(validate_glued_equals_split);
    RUN_TEST(builtin_route_data_available);

    print_test_summary();
    return g_tests_failed;
}
