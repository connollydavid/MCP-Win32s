/*
 * file_ops.h - File read/write/list/delete for MCP-Win32s
 *
 * Win32s-compatible API (ANSI only, no Unicode). C89.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef FILE_OPS_H
#define FILE_OPS_H

#include <windows.h>
#include "common.h"

/* All return 1 on success, 0 on failure. On failure, errMsg gets a short reason. */

/*
 * FileOpRead - Read file into raw buffer.
 *
 * *outLen set to bytes read. Caller provides buf of bufSize bytes.
 * Returns 0 with "file too large" if file exceeds bufSize.
 */
int FileOpRead(const char *path, unsigned char *buf, int bufSize,
               int *outLen, char *errMsg, int errSize);

/*
 * FileOpWrite - Write raw bytes to file (CREATE_ALWAYS).
 */
int FileOpWrite(const char *path, const unsigned char *data, int dataLen,
                char *errMsg, int errSize);

/*
 * FileOpList - List directory contents.
 *
 * Output: newline-separated entries. Directories suffixed with '\'.
 * Skips '.' and '..'. No trailing newline.
 */
int FileOpList(const char *path, char *out, int outSize,
               char *errMsg, int errSize);

/*
 * FileOpDelete - Delete file.
 */
int FileOpDelete(const char *path, char *errMsg, int errSize);

/*
 * FileOpCopy - Copy a file, never overwriting (CopyFileA fail-if-exists).
 *
 * Deliberately non-destructive: overwriting is an explicit
 * delete-then-copy (spec: file-ops.allium FileCopyDestExists).
 * errMsg: "file not found" (source missing), "file exists" (destination
 * present); other host failures get a short implementation-defined reason.
 */
int FileOpCopy(const char *src, const char *dest, char *errMsg, int errSize);

/*
 * FileOpMove - Rename/move a file (MoveFileA; fails if dest exists).
 *
 * errMsg: "file not found" (source missing), "file exists" (destination
 * present); other host failures get a short implementation-defined reason.
 */
int FileOpMove(const char *src, const char *dest, char *errMsg, int errSize);

/*
 * FileOpMakeDir - Create one directory level (CreateDirectoryA).
 *
 * Single level only - no recursive (mkdir -p) creation; a missing parent
 * fails with "path not found".
 * errMsg: "directory exists" (already present), "path not found"
 * (parent missing); other host failures implementation-defined.
 */
int FileOpMakeDir(const char *path, char *errMsg, int errSize);

/*
 * FileOpRemoveDir - Remove an empty directory (RemoveDirectoryA).
 *
 * Refuses non-empty directories; there is deliberately no recursive
 * delete.
 * errMsg: "directory not found" (missing), "directory not empty";
 * other host failures implementation-defined.
 */
int FileOpRemoveDir(const char *path, char *errMsg, int errSize);

#endif /* FILE_OPS_H */
