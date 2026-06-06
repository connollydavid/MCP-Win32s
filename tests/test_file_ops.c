/*
 * test_file_ops.c - Unit tests for file_ops.c
 *
 * Tests file read/write/list/delete with cleanup.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "file_ops.h"

/* Global temp directory path */
static char g_tmpDir[260];

/*
 * tmp_path - Build a temp file path from a name.
 */
static void tmp_path(char *out, int outSize, const char *name)
{
    int dirLen;
    int nameLen;

    dirLen = 0;
    while (g_tmpDir[dirLen] != '\0' && dirLen < outSize - 1) {
        out[dirLen] = g_tmpDir[dirLen];
        dirLen++;
    }

    nameLen = 0;
    while (name[nameLen] != '\0' && dirLen + nameLen < outSize - 1) {
        out[dirLen + nameLen] = name[nameLen];
        nameLen++;
    }

    out[dirLen + nameLen] = '\0';
}

/*
 * cleanup - Delete a temp file if it exists.
 */
static void cleanup(const char *name)
{
    char path[260];
    tmp_path(path, sizeof(path), name);
    DeleteFileA(path);
}

/*
 * cleanup_dir - Delete a temp subdirectory.
 */
static void cleanup_dir(const char *name)
{
    char path[260];
    tmp_path(path, sizeof(path), name);
    /* Remove read-only attribute if set */
    {
        DWORD attrs;
        attrs = GetFileAttributesA(path);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            SetFileAttributesA(path, attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }
    RemoveDirectoryA(path);
}

TEST_CASE(write_then_read_text) {
    char path[260];
    char err[128];
    unsigned char buf[256];
    int len;
    int ok;

    tmp_path(path, sizeof(path), "test_rw.txt");
    cleanup("test_rw.txt");

    {
        const unsigned char data[] = "hello world";
        ok = FileOpWrite(path, data, (int)(sizeof(data) - 1), err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "write succeeds");
    }

    ok = FileOpRead(path, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "read succeeds");
    TEST_ASSERT_INT_EQUAL(11, len, "11 bytes read");

    {
        int i;
        const char *expected;
        expected = "hello world";
        for (i = 0; i < 11; i++) {
            if (buf[i] != (unsigned char)expected[i]) {
                TEST_ASSERT_INT_EQUAL(expected[i], buf[i], "byte matches");
            }
        }
    }

    cleanup("test_rw.txt");
}

TEST_CASE(write_then_read_binary_4k) {
    char path[260];
    char err[128];
    unsigned char writeBuf[4096];
    unsigned char readBuf[4096];
    int len;
    int ok;
    int i;

    /* Fill with all 256 byte values cycled */
    for (i = 0; i < 4096; i++) {
        writeBuf[i] = (unsigned char)(i % 256);
    }

    tmp_path(path, sizeof(path), "test_bin.dat");
    cleanup("test_bin.dat");

    ok = FileOpWrite(path, writeBuf, 4096, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "write 4K succeeds");

    ok = FileOpRead(path, readBuf, sizeof(readBuf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "read succeeds");
    TEST_ASSERT_INT_EQUAL(4096, len, "4096 bytes read");

    for (i = 0; i < 4096; i++) {
        if (readBuf[i] != (unsigned char)(i % 256)) {
            TEST_ASSERT_INT_EQUAL(i % 256, readBuf[i], "byte matches");
        }
    }

    cleanup("test_bin.dat");
}

TEST_CASE(read_nonexistent) {
    char path[260];
    char err[128];
    unsigned char buf[64];
    int len;
    int ok;

    tmp_path(path, sizeof(path), "no_such_file.txt");

    ok = FileOpRead(path, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "read nonexistent returns 0");
    TEST_ASSERT_INT_EQUAL(0, (int)(err[0] == '\0'), "err message non-empty");
}

TEST_CASE(delete_existing) {
    char path[260];
    char err[128];
    int ok;
    int len;

    tmp_path(path, sizeof(path), "test_del.txt");
    cleanup("test_del.txt");

    {
        const unsigned char data[] = "delete me";
        FileOpWrite(path, data, (int)(sizeof(data) - 1), err, sizeof(err));
    }

    ok = FileOpDelete(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "delete succeeds");

    /* Verify file is gone */
    ok = FileOpRead(path, (unsigned char *)err, sizeof(err),
                    &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "read after delete fails");
}

TEST_CASE(delete_nonexistent) {
    char path[260];
    char err[128];
    int ok;

    tmp_path(path, sizeof(path), "no_such_delete.txt");

    ok = FileOpDelete(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "delete nonexistent returns 0");
}

TEST_CASE(list_contains_file) {
    char path[260];
    char err[128];
    char listing[65536];
    int ok;
    int found;

    tmp_path(path, sizeof(path), "test_list.txt");
    cleanup("test_list.txt");

    {
        const unsigned char data[] = "in listing";
        FileOpWrite(path, data, (int)(sizeof(data) - 1), err, sizeof(err));
    }

    tmp_path(path, sizeof(path), "");
    ok = FileOpList(path, listing, sizeof(listing), err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "list succeeds");

    /* Check that our file appears in the listing */
    found = 0;
    {
        const char *p;
        p = strstr(listing, "test_list.txt");
        if (p != NULL) {
            found = 1;
        }
    }
    TEST_ASSERT_INT_EQUAL(1, found, "file found in listing");

    cleanup("test_list.txt");
}

/*
 * Obligation: file-ops.allium rule FileListNotFound — an empty directory
 * path is an error ("directory not found"), never a listing of the
 * server's current working directory (weed 2026-06-06, finding #1).
 */
TEST_CASE(list_empty_path_errors) {
    char err[128];
    char listing[256];
    int ok;

    listing[0] = '\0';
    err[0] = '\0';

    ok = FileOpList("", listing, sizeof(listing), err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "list empty path returns 0");
    TEST_ASSERT_STR_EQUAL("directory not found", err,
                          "empty path reports directory not found");
    TEST_ASSERT_INT_EQUAL(0, (int)listing[0],
                          "no listing produced for empty path");
}

TEST_CASE(list_nonexistent_dir) {
    char path[260];
    char err[128];
    char listing[256];
    int ok;

    tmp_path(path, sizeof(path), "no_such_dir*");

    ok = FileOpList(path, listing, sizeof(listing), err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "list nonexistent returns 0");
}

TEST_CASE(overwrite_file) {
    char path[260];
    char err[128];
    unsigned char buf[256];
    int len;
    int ok;

    tmp_path(path, sizeof(path), "test_overwrite.txt");
    cleanup("test_overwrite.txt");

    {
        const unsigned char data[] = "this is long content";
        ok = FileOpWrite(path, data, (int)(sizeof(data) - 1), err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "first write succeeds");
    }

    {
        const unsigned char data[] = "short";
        ok = FileOpWrite(path, data, (int)(sizeof(data) - 1), err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "overwrite succeeds");
    }

    ok = FileOpRead(path, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "read succeeds");
    TEST_ASSERT_INT_EQUAL(5, len, "5 bytes (short)");

    cleanup("test_overwrite.txt");
}

TEST_CASE(read_file_too_large) {
    char path[260];
    char err[128];
    unsigned char big[256];
    unsigned char smallBuf[8];
    int len;
    int ok;
    int i;

    /* Write 256 bytes */
    for (i = 0; i < 256; i++) {
        big[i] = (unsigned char)i;
    }

    tmp_path(path, sizeof(path), "test_big.dat");
    cleanup("test_big.dat");

    ok = FileOpWrite(path, big, 256, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "write succeeds");

    /* Try to read into 8-byte buffer */
    ok = FileOpRead(path, smallBuf, sizeof(smallBuf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "read too large returns 0");

    cleanup("test_big.dat");
}

TEST_CASE(list_dir_has_subdir) {
    char path[260];
    char dirPath[260];
    char err[128];
    char listing[65536];
    int ok;
    int found;

    tmp_path(dirPath, sizeof(dirPath), "test_subdir");
    cleanup_dir("test_subdir");

    tmp_path(path, sizeof(path), "test_subdir");
    ok = CreateDirectoryA(path, NULL) != 0;
    TEST_ASSERT_INT_EQUAL(1, ok, "create subdir succeeds");

    tmp_path(path, sizeof(path), "");
    ok = FileOpList(path, listing, sizeof(listing), err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "list succeeds");

    /* Check that subdir appears with '\' suffix */
    found = 0;
    {
        const char *p;
        p = strstr(listing, "test_subdir\\");
        if (p != NULL) {
            found = 1;
        }
    }
    TEST_ASSERT_INT_EQUAL(1, found, "subdir found with trailing \\");

    cleanup_dir("test_subdir");
}

int main(void)
{
    char tmpDir[260];

    /* Get temp directory */
    {
        DWORD len;
        len = GetTempPathA(sizeof(tmpDir), tmpDir);
        if (len == 0) {
            /* Fallback */
            tmpDir[0] = 'C';
            tmpDir[1] = ':';
            tmpDir[2] = '\\';
            tmpDir[3] = 'T';
            tmpDir[4] = 'E';
            tmpDir[5] = 'M';
            tmpDir[6] = 'P';
            tmpDir[7] = '\\';
            tmpDir[8] = '\0';
        }
    }

    /* Copy to global */
    {
        int i;
        for (i = 0; tmpDir[i] != '\0' && i < 259; i++) {
            g_tmpDir[i] = tmpDir[i];
        }
        g_tmpDir[i] = '\0';
    }

    printf("  Temp dir: %s\n\n", g_tmpDir);

    RUN_TEST(write_then_read_text);
    RUN_TEST(write_then_read_binary_4k);
    RUN_TEST(read_nonexistent);
    RUN_TEST(delete_existing);
    RUN_TEST(delete_nonexistent);
    RUN_TEST(list_contains_file);
    RUN_TEST(list_empty_path_errors);
    RUN_TEST(list_nonexistent_dir);
    RUN_TEST(overwrite_file);
    RUN_TEST(read_file_too_large);
    RUN_TEST(list_dir_has_subdir);

    print_test_summary();
    return g_tests_failed;
}
