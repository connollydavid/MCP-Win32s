/*
 * file_ops.c - File operations for MCP-Win32s
 *
 * Tier-aware text encoding (work-item 5.4): every inbound agent path is a
 * UTF-8 wire path, prepared for the host's file API by EncOpenPath. On the
 * `wide` tier (NT family) the ops call the delay-loaded -W file APIs over the
 * widened UTF-16 (g_features.p*W); on the `manifest`/`codepage` tiers they call
 * the -A APIs over EncOpenPath's mbOut (UTF-8 on the manifest tier, the narrowed
 * ANSI page on the codepage tier). An unrepresentable codepage-tier path is
 * REJECTED before any file is touched (StrictNarrowingRejectsUnrepresentable).
 * Directory names/listings are rendered back to the UTF-8 wire through
 * EncWideToWire/EncBytesToWire; file READ/WRITE DATA stays raw bytes (never
 * transcoded). Spec: file-ops.allium, encoding.allium. C89, i386.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "file_ops.h"
#include "encoding.h"
#include "feat.h"

/*
 * err_set - Set error message with bounds checking.
 */
static void err_set(char *msg, int msgSize, const char *text)
{
    int i;
    int len;

    if (msg == NULL || msgSize <= 0 || text == NULL) {
        return;
    }

    len = 0;
    while (text[len] != '\0') {
        len++;
    }

    if (len >= msgSize) {
        len = msgSize - 1;
    }

    for (i = 0; i < len; i++) {
        msg[i] = text[i];
    }
    msg[len] = '\0';
}

/*
 * str_endswith - Check if string ends with given suffix.
 */
static int str_endswith(const char *s, const char *suffix)
{
    int s_len;
    int suf_len;
    int i;

    s_len = 0;
    while (s[s_len] != '\0') {
        s_len++;
    }

    suf_len = 0;
    while (suffix[suf_len] != '\0') {
        suf_len++;
    }

    if (suf_len > s_len) {
        return 0;
    }

    for (i = 0; i < suf_len; i++) {
        if (s[s_len - suf_len + i] != suffix[i]) {
            return 0;
        }
    }

    return 1;
}

/*
 * enc_prep_path - EncOpenPath plus the strict-narrowing reject gate.
 *
 * Returns 1 with *form populated for the current tier (useWide + wOut/mbOut).
 * Returns 0 (errMsg = "path not representable") when the path cannot be
 * represented in the codepage tier's ANSI page - the caller must then touch no
 * file (StrictNarrowingRejectsUnrepresentable). A clean lossy widen on the
 * codepage tier is NOT a reject (status ENC_LOSSY still yields usable mbOut).
 */
static int enc_prep_path(const char *path, EncPathForm *form,
                         char *errMsg, int errSize)
{
    EncOpenPath(path, form);
    if (form->status == ENC_REJECTED) {
        err_set(errMsg, errSize, "path not representable");
        return 0;
    }
    return 1;
}

int FileOpRead(const char *path, unsigned char *buf, int bufSize,
               int *outLen, char *errMsg, int errSize)
{
    HANDLE hFile;
    LARGE_INTEGER fileSize;
    DWORD bytesRead;
    int total;
    EncPathForm form;

    if (path == NULL || buf == NULL || outLen == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    *outLen = 0;

    if (!enc_prep_path(path, &form, errMsg, errSize)) {
        return 0;
    }

    if (form.useWide) {
        hFile = g_features.pCreateFileW(form.wOut, GENERIC_READ,
                                        FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        hFile = CreateFileA(form.mbOut, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        err_set(errMsg, errSize, "file not found");
        return 0;
    }

    /* GetFileSizeEx is absent from the Win32s 1.25a thunk (w32scomb.dll);
       GetFileSize is exported and sufficient here (files are bufSize-bound,
       well under the 16 MB per-app limit). Per its contract, INVALID_FILE_SIZE
       is only an error when GetLastError() is not NO_ERROR. */
    fileSize.HighPart = 0;
    fileSize.LowPart = GetFileSize(hFile, NULL);
    if (fileSize.LowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        err_set(errMsg, errSize, "cannot get file size");
        CloseHandle(hFile);
        return 0;
    }

    if ((int)fileSize.LowPart > bufSize) {
        err_set(errMsg, errSize, "file too large");
        CloseHandle(hFile);
        return 0;
    }

    total = 0;
    while (total < (int)fileSize.LowPart) {
        int toRead;
        toRead = (int)fileSize.LowPart - total;
        if (toRead > bufSize - total) {
            toRead = bufSize - total;
        }

        if (!ReadFile(hFile, buf + total, (DWORD)toRead, &bytesRead, NULL)) {
            err_set(errMsg, errSize, "read error");
            CloseHandle(hFile);
            return 0;
        }

        if (bytesRead == 0) {
            break;
        }

        total += (int)bytesRead;
    }

    *outLen = total;
    CloseHandle(hFile);
    return 1;
}

int FileOpWrite(const char *path, const unsigned char *data, int dataLen,
                char *errMsg, int errSize)
{
    HANDLE hFile;
    DWORD bytesWritten;
    EncPathForm form;

    if (path == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!enc_prep_path(path, &form, errMsg, errSize)) {
        return 0;
    }

    if (form.useWide) {
        hFile = g_features.pCreateFileW(form.wOut, GENERIC_WRITE, 0, NULL,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                        NULL);
    } else {
        hFile = CreateFileA(form.mbOut, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        err_set(errMsg, errSize, "cannot create file");
        return 0;
    }

    if (dataLen > 0) {
        if (!WriteFile(hFile, data, (DWORD)dataLen, &bytesWritten, NULL)) {
            err_set(errMsg, errSize, "write error");
            CloseHandle(hFile);
            return 0;
        }

        if ((int)bytesWritten != dataLen) {
            err_set(errMsg, errSize, "incomplete write");
            CloseHandle(hFile);
            return 0;
        }
    }

    CloseHandle(hFile);
    return 1;
}

int FileOpList(const char *path, char *out, int outSize,
               char *errMsg, int errSize)
{
    /* Single-threaded server (Win32s): the large find-data + path form are
     * static to keep the stack small. */
    static EncPathForm form;
    static WIN32_FIND_DATAW findW;
    WIN32_FIND_DATAA findA;
    char searchU8[ENC_PATH_MB];
    char nameUtf8[MCP_MAX_PATH_LEN * 4];
    HANDLE hFind;
    int searchLen;
    int outPos;
    int first;
    int useWide;
    int i;

    if (path == NULL || out == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    /*
     * An empty path is an error, not the current directory: building the
     * search path from "" would yield "*" and silently list the server's
     * CWD (spec: file-ops.allium rule FileListNotFound).
     */
    if (path[0] == '\0') {
        err_set(errMsg, errSize, "directory not found");
        return 0;
    }

    /*
     * Build the search pattern "<path>\\*" on the UTF-8 wire form, BEFORE the
     * single narrowing point. The '\\' and '*' appended are ASCII, which (UTF-8
     * being self-synchronising) can never be a DBCS trail byte - so the pattern
     * build is DBCS-safe by construction (PathSeparatorScanIsDbcsSafe).
     */
    searchLen = 0;
    for (i = 0; path[i] != '\0' && searchLen < ENC_PATH_MB - 4; i++) {
        searchU8[searchLen++] = path[i];
    }
    searchU8[searchLen] = '\0';

    if (searchLen > 0 && !str_endswith(searchU8, "\\") &&
        !str_endswith(searchU8, "*")) {
        searchU8[searchLen++] = '\\';
        searchU8[searchLen] = '\0';
    }
    if (!str_endswith(searchU8, "*")) {
        searchU8[searchLen++] = '*';
        searchU8[searchLen] = '\0';
    }

    /* A listing pattern that cannot be represented on the codepage tier just
     * has no such directory; no strict reject error is needed here. */
    EncOpenPath(searchU8, &form);
    if (form.status == ENC_REJECTED) {
        err_set(errMsg, errSize, "directory not found");
        return 0;
    }
    useWide = form.useWide;

    if (useWide) {
        hFind = g_features.pFindFirstFileW(form.wOut, &findW);
    } else {
        hFind = FindFirstFileA(form.mbOut, &findA);
    }

    if (hFind == INVALID_HANDLE_VALUE) {
        err_set(errMsg, errSize, "directory not found");
        return 0;
    }

    outPos = 0;
    first = 1;

    do {
        int isDir;
        int nameLen;

        /*
         * Render the OS-sourced name back to the UTF-8 wire (wide: UTF-16 via
         * EncWideToWire; -A: the active code page via EncBytesToWire). '.' and
         * '..' are ASCII on either form, so the skip test is identical.
         */
        if (useWide) {
            int u;
            if (findW.cFileName[0] == '.' &&
                (findW.cFileName[1] == '\0' ||
                 (findW.cFileName[1] == '.' && findW.cFileName[2] == '\0'))) {
                continue;
            }
            isDir = (findW.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            u = 0;
            while (findW.cFileName[u] != 0) {
                u++;
            }
            nameLen = EncWideToWire((const unsigned short *)findW.cFileName, u,
                                    nameUtf8, (int)sizeof(nameUtf8), NULL);
        } else {
            int b;
            if (findA.cFileName[0] == '.' &&
                (findA.cFileName[1] == '\0' ||
                 (findA.cFileName[1] == '.' && findA.cFileName[2] == '\0'))) {
                continue;
            }
            isDir = (findA.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            b = 0;
            while (findA.cFileName[b] != '\0') {
                b++;
            }
            nameLen = EncBytesToWire(EncActiveCodePage(),
                                     (const unsigned char *)findA.cFileName, b,
                                     nameUtf8, (int)sizeof(nameUtf8), NULL);
        }

        {
            int needed;
            needed = nameLen;
            if (isDir) needed++;
            if (!first) needed++;
            needed++; /* NUL */

            if (outPos + needed > outSize) {
                FindClose(hFind);
                err_set(errMsg, errSize, "buffer overflow");
                return 0;
            }
        }

        /* Separator between entries */
        if (!first) {
            out[outPos++] = '\n';
        }
        first = 0;

        /* Entry name (UTF-8) */
        for (i = 0; i < nameLen; i++) {
            out[outPos++] = nameUtf8[i];
        }

        /* Directory suffix */
        if (isDir) {
            out[outPos++] = '\\';
        }
    } while (useWide ? g_features.pFindNextFileW(hFind, &findW)
                     : FindNextFileA(hFind, &findA));

    FindClose(hFind);
    out[outPos] = '\0';

    /* first still set -> empty directory: success with empty output. */
    return 1;
}

int FileOpDelete(const char *path, char *errMsg, int errSize)
{
    EncPathForm form;
    int ok;

    if (path == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!enc_prep_path(path, &form, errMsg, errSize)) {
        return 0;
    }

    if (form.useWide) {
        ok = g_features.pDeleteFileW(form.wOut);
    } else {
        ok = DeleteFileA(form.mbOut);
    }

    if (!ok) {
        err_set(errMsg, errSize, "delete failed");
        return 0;
    }

    return 1;
}

int FileOpCopy(const char *src, const char *dest, char *errMsg, int errSize)
{
    EncPathForm srcForm;
    EncPathForm destForm;
    DWORD lastErr;
    int ok;

    if (src == NULL || dest == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!enc_prep_path(src, &srcForm, errMsg, errSize)) {
        return 0;
    }
    if (!enc_prep_path(dest, &destForm, errMsg, errSize)) {
        return 0;
    }

    /* TRUE = fail if dest exists: deliberately never overwrite. */
    if (srcForm.useWide) {
        ok = g_features.pCopyFileW(srcForm.wOut, destForm.wOut, TRUE);
    } else {
        ok = CopyFileA(srcForm.mbOut, destForm.mbOut, TRUE);
    }
    if (ok) {
        return 1;
    }

    lastErr = GetLastError();
    if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND) {
        err_set(errMsg, errSize, "file not found");
    } else if (lastErr == ERROR_FILE_EXISTS ||
               lastErr == ERROR_ALREADY_EXISTS) {
        err_set(errMsg, errSize, "file exists");
    } else {
        err_set(errMsg, errSize, "copy failed");
    }
    return 0;
}

int FileOpMove(const char *src, const char *dest, char *errMsg, int errSize)
{
    EncPathForm srcForm;
    EncPathForm destForm;
    DWORD lastErr;
    int ok;

    if (src == NULL || dest == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!enc_prep_path(src, &srcForm, errMsg, errSize)) {
        return 0;
    }
    if (!enc_prep_path(dest, &destForm, errMsg, errSize)) {
        return 0;
    }

    /* MoveFileA/W fails if dest exists (no overwrite flag). */
    if (srcForm.useWide) {
        ok = g_features.pMoveFileW(srcForm.wOut, destForm.wOut);
    } else {
        ok = MoveFileA(srcForm.mbOut, destForm.mbOut);
    }
    if (ok) {
        return 1;
    }

    lastErr = GetLastError();
    if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND) {
        err_set(errMsg, errSize, "file not found");
    } else if (lastErr == ERROR_ALREADY_EXISTS ||
               lastErr == ERROR_FILE_EXISTS) {
        err_set(errMsg, errSize, "file exists");
    } else {
        err_set(errMsg, errSize, "move failed");
    }
    return 0;
}

int FileOpMakeDir(const char *path, char *errMsg, int errSize)
{
    EncPathForm form;
    DWORD lastErr;
    int ok;

    if (path == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!enc_prep_path(path, &form, errMsg, errSize)) {
        return 0;
    }

    /* Single level only: a missing parent fails, never mkdir -p. */
    if (form.useWide) {
        ok = g_features.pCreateDirectoryW(form.wOut, NULL);
    } else {
        ok = CreateDirectoryA(form.mbOut, NULL);
    }
    if (ok) {
        return 1;
    }

    lastErr = GetLastError();
    if (lastErr == ERROR_ALREADY_EXISTS) {
        err_set(errMsg, errSize, "directory exists");
    } else if (lastErr == ERROR_PATH_NOT_FOUND) {
        err_set(errMsg, errSize, "path not found");
    } else {
        err_set(errMsg, errSize, "mkdir failed");
    }
    return 0;
}

int FileOpRemoveDir(const char *path, char *errMsg, int errSize)
{
    EncPathForm form;
    DWORD lastErr;
    int ok;

    if (path == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!enc_prep_path(path, &form, errMsg, errSize)) {
        return 0;
    }

    /* Refuses non-empty directories: no recursive delete. */
    if (form.useWide) {
        ok = g_features.pRemoveDirectoryW(form.wOut);
    } else {
        ok = RemoveDirectoryA(form.mbOut);
    }
    if (ok) {
        return 1;
    }

    lastErr = GetLastError();
    if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND) {
        err_set(errMsg, errSize, "directory not found");
    } else if (lastErr == ERROR_DIR_NOT_EMPTY) {
        err_set(errMsg, errSize, "directory not empty");
    } else {
        err_set(errMsg, errSize, "rmdir failed");
    }
    return 0;
}
