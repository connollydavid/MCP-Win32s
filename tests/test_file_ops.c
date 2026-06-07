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

/*
 * Obligations: rule-success.FileCopySuccess, rule-failure.FileCopySuccess.{1,2},
 * entity-fields.FileCopyResult, transition-{rejected,terminal}.FileCopyResult.status
 * (OBLIGATIONS-5.1.md, Device: copy). Copy to a missing dest succeeds; dest is
 * readable with identical content; the source survives.
 */
TEST_CASE(copy_creates_dest) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    unsigned char buf[256];
    int len;
    int ok;

    tmp_path(srcPath, sizeof(srcPath), "test_cp_src.txt");
    tmp_path(destPath, sizeof(destPath), "test_cp_dst.txt");
    cleanup("test_cp_src.txt");
    cleanup("test_cp_dst.txt");

    {
        const unsigned char data[] = "copy me please";
        ok = FileOpWrite(srcPath, data, (int)(sizeof(data) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "source write succeeds");
    }

    ok = FileOpCopy(srcPath, destPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "copy to missing dest succeeds");

    ok = FileOpRead(destPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "dest readable");
    TEST_ASSERT_INT_EQUAL(14, len, "dest has source length");
    {
        int i;
        const char *expected;
        expected = "copy me please";
        for (i = 0; i < 14; i++) {
            if (buf[i] != (unsigned char)expected[i]) {
                TEST_ASSERT_INT_EQUAL(expected[i], buf[i], "dest byte matches");
            }
        }
    }

    /* Source must still be present after the copy. */
    ok = FileOpRead(srcPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "source still present");

    cleanup("test_cp_src.txt");
    cleanup("test_cp_dst.txt");
}

/*
 * Obligations: rule-success.FileCopySourceMissing,
 * rule-failure.FileCopySourceMissing.1, when-presence.FileCopyResult.error_reason
 * (OBLIGATIONS-5.1.md, Device: copy). Missing source reports exactly
 * "file not found".
 */
TEST_CASE(copy_source_missing_errors) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    int ok;

    tmp_path(srcPath, sizeof(srcPath), "no_such_cp_src.txt");
    tmp_path(destPath, sizeof(destPath), "test_cp_dst2.txt");
    cleanup("no_such_cp_src.txt");
    cleanup("test_cp_dst2.txt");

    err[0] = '\0';
    ok = FileOpCopy(srcPath, destPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "copy missing source returns 0");
    TEST_ASSERT_STR_EQUAL("file not found", err, "reason is file not found");
}

/*
 * Obligations: rule-success.FileCopyDestExists,
 * rule-failure.FileCopyDestExists.{1,2} (OBLIGATIONS-5.1.md, Device: copy).
 * The fail-if-exists pin: reason exactly "file exists" and dest content is
 * untouched. Also covers src=dest (same path resolves to dest exists).
 */
TEST_CASE(copy_dest_exists_errors) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    unsigned char buf[256];
    int len;
    int ok;

    tmp_path(srcPath, sizeof(srcPath), "test_cp_src3.txt");
    tmp_path(destPath, sizeof(destPath), "test_cp_dst3.txt");
    cleanup("test_cp_src3.txt");
    cleanup("test_cp_dst3.txt");

    {
        const unsigned char sdata[] = "new source content";
        ok = FileOpWrite(srcPath, sdata, (int)(sizeof(sdata) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "source write succeeds");
    }
    {
        const unsigned char ddata[] = "ORIGINAL";
        ok = FileOpWrite(destPath, ddata, (int)(sizeof(ddata) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "dest write succeeds");
    }

    err[0] = '\0';
    ok = FileOpCopy(srcPath, destPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "copy onto existing dest returns 0");
    TEST_ASSERT_STR_EQUAL("file exists", err, "reason is file exists");

    /* Dest content must be untouched (never overwritten). */
    ok = FileOpRead(destPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "dest still readable");
    TEST_ASSERT_INT_EQUAL(8, len, "dest length unchanged");
    {
        int i;
        const char *expected;
        expected = "ORIGINAL";
        for (i = 0; i < 8; i++) {
            if (buf[i] != (unsigned char)expected[i]) {
                TEST_ASSERT_INT_EQUAL(expected[i], buf[i],
                                      "dest byte untouched");
            }
        }
    }

    /* Copying a path onto itself also fails as dest exists. */
    err[0] = '\0';
    ok = FileOpCopy(srcPath, srcPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "copy src onto itself returns 0");
    TEST_ASSERT_STR_EQUAL("file exists", err, "self-copy reports file exists");

    cleanup("test_cp_src3.txt");
    cleanup("test_cp_dst3.txt");
}

/*
 * Obligation: FileCopySuccess content fidelity (OBLIGATIONS-5.1.md,
 * Device: copy, prop). Fixed-seed pseudo-random binary buffers across varying
 * lengths (including 0 and odd sizes) survive write->copy->read byte-identical.
 */
TEST_CASE(copy_preserves_content) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    unsigned char writeBuf[512];
    unsigned char readBuf[512];
    unsigned long seed;
    int iter;

    tmp_path(srcPath, sizeof(srcPath), "test_cp_prop_src.dat");
    tmp_path(destPath, sizeof(destPath), "test_cp_prop_dst.dat");

    seed = 0x12345678UL;
    for (iter = 0; iter < 100; iter++) {
        int dataLen;
        int i;
        int ok;
        int rdLen;

        /* Vary length: 0, odd, and larger sizes up to 511. */
        dataLen = (int)(seed % 512UL);

        for (i = 0; i < dataLen; i++) {
            /* Linear congruential generator (Numerical Recipes constants). */
            seed = seed * 1664525UL + 1013904223UL;
            writeBuf[i] = (unsigned char)((seed >> 16) & 0xFFUL);
        }
        /* Advance the seed once more for the next iteration's length. */
        seed = seed * 1664525UL + 1013904223UL;

        cleanup("test_cp_prop_src.dat");
        cleanup("test_cp_prop_dst.dat");

        ok = FileOpWrite(srcPath, writeBuf, dataLen, err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "prop source write succeeds");

        ok = FileOpCopy(srcPath, destPath, err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "prop copy succeeds");

        rdLen = -1;
        ok = FileOpRead(destPath, readBuf, sizeof(readBuf), &rdLen,
                        err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "prop dest readable");
        TEST_ASSERT_INT_EQUAL(dataLen, rdLen, "prop dest length matches");

        for (i = 0; i < dataLen; i++) {
            if (readBuf[i] != writeBuf[i]) {
                TEST_ASSERT_INT_EQUAL(writeBuf[i], readBuf[i],
                                      "prop dest byte identical");
            }
        }
    }

    cleanup("test_cp_prop_src.dat");
    cleanup("test_cp_prop_dst.dat");
}

/*
 * Obligations: rule-success.FileMoveSuccess, rule-failure.FileMoveSuccess.{1,2},
 * entity-fields.FileMoveResult, transition-{rejected,terminal}.FileMoveResult.status
 * (OBLIGATIONS-5.1.md, Device: move). Move to a missing dest: dest gets the
 * source content; the source is gone.
 */
TEST_CASE(move_renames) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    unsigned char buf[256];
    int len;
    int ok;

    tmp_path(srcPath, sizeof(srcPath), "test_mv_src.txt");
    tmp_path(destPath, sizeof(destPath), "test_mv_dst.txt");
    cleanup("test_mv_src.txt");
    cleanup("test_mv_dst.txt");

    {
        const unsigned char data[] = "move this content";
        ok = FileOpWrite(srcPath, data, (int)(sizeof(data) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "source write succeeds");
    }

    ok = FileOpMove(srcPath, destPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "move to missing dest succeeds");

    ok = FileOpRead(destPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "dest readable");
    TEST_ASSERT_INT_EQUAL(17, len, "dest has source length");
    {
        int i;
        const char *expected;
        expected = "move this content";
        for (i = 0; i < 17; i++) {
            if (buf[i] != (unsigned char)expected[i]) {
                TEST_ASSERT_INT_EQUAL(expected[i], buf[i], "dest byte matches");
            }
        }
    }

    /* Source must be gone after the move. */
    ok = FileOpRead(srcPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "source gone after move");

    cleanup("test_mv_src.txt");
    cleanup("test_mv_dst.txt");
}

/*
 * Obligations: rule-success.FileMoveSourceMissing,
 * rule-failure.FileMoveSourceMissing.1, when-presence.FileMoveResult.error_reason
 * (OBLIGATIONS-5.1.md, Device: move). Missing source reports exactly
 * "file not found".
 */
TEST_CASE(move_source_missing_errors) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    int ok;

    tmp_path(srcPath, sizeof(srcPath), "no_such_mv_src.txt");
    tmp_path(destPath, sizeof(destPath), "test_mv_dst2.txt");
    cleanup("no_such_mv_src.txt");
    cleanup("test_mv_dst2.txt");

    err[0] = '\0';
    ok = FileOpMove(srcPath, destPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "move missing source returns 0");
    TEST_ASSERT_STR_EQUAL("file not found", err, "reason is file not found");
}

/*
 * Obligations: rule-success.FileMoveDestExists,
 * rule-failure.FileMoveDestExists.{1,2} (OBLIGATIONS-5.1.md, Device: move).
 * MoveFileA rename semantics: reason exactly "file exists" and both files
 * untouched.
 */
TEST_CASE(move_dest_exists_errors) {
    char srcPath[260];
    char destPath[260];
    char err[128];
    unsigned char buf[256];
    int len;
    int ok;

    tmp_path(srcPath, sizeof(srcPath), "test_mv_src3.txt");
    tmp_path(destPath, sizeof(destPath), "test_mv_dst3.txt");
    cleanup("test_mv_src3.txt");
    cleanup("test_mv_dst3.txt");

    {
        const unsigned char sdata[] = "SOURCE3";
        ok = FileOpWrite(srcPath, sdata, (int)(sizeof(sdata) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "source write succeeds");
    }
    {
        const unsigned char ddata[] = "DEST3DATA";
        ok = FileOpWrite(destPath, ddata, (int)(sizeof(ddata) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "dest write succeeds");
    }

    err[0] = '\0';
    ok = FileOpMove(srcPath, destPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "move onto existing dest returns 0");
    TEST_ASSERT_STR_EQUAL("file exists", err, "reason is file exists");

    /* Source untouched. */
    ok = FileOpRead(srcPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "source still readable");
    TEST_ASSERT_INT_EQUAL(7, len, "source length unchanged");
    {
        int i;
        const char *expected;
        expected = "SOURCE3";
        for (i = 0; i < 7; i++) {
            if (buf[i] != (unsigned char)expected[i]) {
                TEST_ASSERT_INT_EQUAL(expected[i], buf[i],
                                      "source byte untouched");
            }
        }
    }

    /* Dest untouched. */
    ok = FileOpRead(destPath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "dest still readable");
    TEST_ASSERT_INT_EQUAL(9, len, "dest length unchanged");
    {
        int i;
        const char *expected;
        expected = "DEST3DATA";
        for (i = 0; i < 9; i++) {
            if (buf[i] != (unsigned char)expected[i]) {
                TEST_ASSERT_INT_EQUAL(expected[i], buf[i],
                                      "dest byte untouched");
            }
        }
    }

    cleanup("test_mv_src3.txt");
    cleanup("test_mv_dst3.txt");
}

/*
 * Obligations: rule-success.MakeDirSuccess, rule-failure.MakeDirSuccess.1,
 * entity-fields.MakeDirResult, transition-{rejected,terminal}.MakeDirResult.status,
 * transition-edge.Directory.missing.present (OBLIGATIONS-5.1.md, Device: mkdir).
 * mkdir on a missing path succeeds; the parent listing shows the new dir
 * (directories suffixed '\' per FileOpList).
 */
TEST_CASE(mkdir_creates) {
    char path[260];
    char err[128];
    char listing[65536];
    int ok;
    int found;

    cleanup_dir("test_mkdir_new");

    tmp_path(path, sizeof(path), "test_mkdir_new");
    err[0] = '\0';
    ok = FileOpMakeDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "mkdir on missing path succeeds");

    tmp_path(path, sizeof(path), "");
    ok = FileOpList(path, listing, sizeof(listing), err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "parent list succeeds");

    found = 0;
    {
        const char *p;
        p = strstr(listing, "test_mkdir_new\\");
        if (p != NULL) {
            found = 1;
        }
    }
    TEST_ASSERT_INT_EQUAL(1, found, "new dir found with trailing \\");

    cleanup_dir("test_mkdir_new");
}

/*
 * Obligations: rule-success.MakeDirAlreadyExists,
 * rule-failure.MakeDirAlreadyExists.1, when-presence.MakeDirResult.error_reason
 * (OBLIGATIONS-5.1.md, Device: mkdir). mkdir on an existing dir reports exactly
 * "directory exists".
 */
TEST_CASE(mkdir_existing_errors) {
    char path[260];
    char err[128];
    int ok;

    cleanup_dir("test_mkdir_exist");

    tmp_path(path, sizeof(path), "test_mkdir_exist");
    ok = FileOpMakeDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "first mkdir succeeds");

    err[0] = '\0';
    ok = FileOpMakeDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "second mkdir returns 0");
    TEST_ASSERT_STR_EQUAL("directory exists", err,
                          "reason is directory exists");

    cleanup_dir("test_mkdir_exist");
}

/*
 * Obligation: spec comment pin — single level only (OBLIGATIONS-5.1.md,
 * Device: mkdir). mkdir of "a\b" with parent "a" missing FAILS (any reason);
 * guards against an accidental mkdir -p.
 */
TEST_CASE(mkdir_no_recursive_create) {
    char path[260];
    char err[128];
    int ok;

    /* Parent "test_mkdir_norec" is deliberately never created. */
    cleanup_dir("test_mkdir_norec\\child");
    cleanup_dir("test_mkdir_norec");

    tmp_path(path, sizeof(path), "test_mkdir_norec\\child");
    err[0] = '\0';
    ok = FileOpMakeDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "mkdir with missing parent fails");
    TEST_ASSERT_INT_EQUAL(0, (int)(err[0] == '\0'), "err message non-empty");
}

/*
 * Obligations: rule-success.RemoveDirSuccess,
 * rule-failure.RemoveDirSuccess.{1,2}, entity-fields.RemoveDirResult,
 * transition-{rejected,terminal}.RemoveDirResult.status,
 * transition-edge.Directory.present.deleted (OBLIGATIONS-5.1.md, Device: rmdir).
 * rmdir on an empty dir succeeds; the parent listing then omits it.
 */
TEST_CASE(rmdir_empty_succeeds) {
    char path[260];
    char err[128];
    char listing[65536];
    int ok;
    int found;

    cleanup_dir("test_rmdir_empty");

    tmp_path(path, sizeof(path), "test_rmdir_empty");
    ok = FileOpMakeDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "mkdir succeeds");

    err[0] = '\0';
    ok = FileOpRemoveDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "rmdir empty dir succeeds");

    tmp_path(path, sizeof(path), "");
    ok = FileOpList(path, listing, sizeof(listing), err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "parent list succeeds");

    found = 0;
    {
        const char *p;
        p = strstr(listing, "test_rmdir_empty\\");
        if (p != NULL) {
            found = 1;
        }
    }
    TEST_ASSERT_INT_EQUAL(0, found, "removed dir absent from listing");

    cleanup_dir("test_rmdir_empty");
}

/*
 * Obligations: rule-success.RemoveDirNotEmpty,
 * rule-failure.RemoveDirNotEmpty.{1,2} (OBLIGATIONS-5.1.md, Device: rmdir).
 * The non-empty refusal pin (no recursive delete): reason exactly
 * "directory not empty"; dir and contents untouched.
 */
TEST_CASE(rmdir_nonempty_errors) {
    char dirPath[260];
    char filePath[260];
    char err[128];
    unsigned char buf[64];
    int len;
    int ok;

    cleanup("test_rmdir_full\\inside.txt");
    cleanup_dir("test_rmdir_full");

    tmp_path(dirPath, sizeof(dirPath), "test_rmdir_full");
    ok = FileOpMakeDir(dirPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "mkdir succeeds");

    tmp_path(filePath, sizeof(filePath), "test_rmdir_full\\inside.txt");
    {
        const unsigned char data[] = "keep me";
        ok = FileOpWrite(filePath, data, (int)(sizeof(data) - 1),
                         err, sizeof(err));
        TEST_ASSERT_INT_EQUAL(1, ok, "file inside dir written");
    }

    err[0] = '\0';
    ok = FileOpRemoveDir(dirPath, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "rmdir non-empty returns 0");
    TEST_ASSERT_STR_EQUAL("directory not empty", err,
                          "reason is directory not empty");

    /* The contained file (and thus the dir) must be untouched. */
    ok = FileOpRead(filePath, buf, sizeof(buf), &len, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "contained file still readable");
    TEST_ASSERT_INT_EQUAL(7, len, "contained file length unchanged");

    cleanup("test_rmdir_full\\inside.txt");
    cleanup_dir("test_rmdir_full");
}

/*
 * Obligations: rule-success.RemoveDirNotFound,
 * rule-failure.RemoveDirNotFound.1, when-presence.RemoveDirResult.error_reason
 * (OBLIGATIONS-5.1.md, Device: rmdir). rmdir on a missing dir reports exactly
 * "directory not found".
 */
TEST_CASE(rmdir_missing_errors) {
    char path[260];
    char err[128];
    int ok;

    cleanup_dir("no_such_rmdir");

    tmp_path(path, sizeof(path), "no_such_rmdir");
    err[0] = '\0';
    ok = FileOpRemoveDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "rmdir missing dir returns 0");
    TEST_ASSERT_STR_EQUAL("directory not found", err,
                          "reason is directory not found");
}

/*
 * Obligations: rule-success.DirectoryCreatedMissing,
 * transition-terminal.Directory.status, transition-rejected.Directory.status
 * (OBLIGATIONS-5.1.md, Directory lifecycle). Full walk missing->present->deleted
 * on one path; rmdir again errors (deleted is terminal — "directory not found").
 */
TEST_CASE(mkdir_then_rmdir_lifecycle) {
    char path[260];
    char err[128];
    int ok;

    cleanup_dir("test_dir_lifecycle");

    tmp_path(path, sizeof(path), "test_dir_lifecycle");

    /* missing -> present */
    ok = FileOpMakeDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "mkdir succeeds (missing->present)");

    /* present -> deleted */
    ok = FileOpRemoveDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, ok, "rmdir succeeds (present->deleted)");

    /* deleted is terminal: rmdir again fails. */
    err[0] = '\0';
    ok = FileOpRemoveDir(path, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, ok, "rmdir on deleted dir returns 0");
    TEST_ASSERT_STR_EQUAL("directory not found", err,
                          "reason is directory not found");

    cleanup_dir("test_dir_lifecycle");
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
    RUN_TEST(copy_creates_dest);
    RUN_TEST(copy_source_missing_errors);
    RUN_TEST(copy_dest_exists_errors);
    RUN_TEST(copy_preserves_content);
    RUN_TEST(move_renames);
    RUN_TEST(move_source_missing_errors);
    RUN_TEST(move_dest_exists_errors);
    RUN_TEST(mkdir_creates);
    RUN_TEST(mkdir_existing_errors);
    RUN_TEST(mkdir_no_recursive_create);
    RUN_TEST(rmdir_empty_succeeds);
    RUN_TEST(rmdir_nonempty_errors);
    RUN_TEST(rmdir_missing_errors);
    RUN_TEST(mkdir_then_rmdir_lifecycle);

    print_test_summary();
    return g_tests_failed;
}
