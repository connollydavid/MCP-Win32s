/*
 * file_ops.c - File operations for MCP-Win32s
 *
 * Win32s-compatible: ANSI APIs only, no Unicode, C89, i386.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "file_ops.h"

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

int FileOpRead(const char *path, unsigned char *buf, int bufSize,
               int *outLen, char *errMsg, int errSize)
{
    HANDLE hFile;
    LARGE_INTEGER fileSize;
    DWORD bytesRead;
    int total;

    if (path == NULL || buf == NULL || outLen == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    *outLen = 0;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        err_set(errMsg, errSize, "file not found");
        return 0;
    }

    if (!GetFileSizeEx(hFile, &fileSize)) {
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

    if (path == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

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
    HANDLE hFind;
    WIN32_FIND_DATAA findData;
    char searchPath[264];
    int searchLen;
    int outPos;
    int first;
    int i;

    if (path == NULL || out == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    /* Build search path: path + "\\*" */
    searchLen = 0;
    for (i = 0; path[i] != '\0' && searchLen < 260; i++) {
        searchPath[searchLen++] = path[i];
    }
    searchPath[searchLen] = '\0';

    /* Append '\\' if not already present */
    if (searchLen > 0 && !str_endswith(searchPath, "\\") &&
        !str_endswith(searchPath, "*")) {
        if (searchLen < 260) {
            searchPath[searchLen++] = '\\';
            searchPath[searchLen] = '\0';
        }
    }

    /* Append '*' if not already present */
    if (!str_endswith(searchPath, "*")) {
        if (searchLen < 260) {
            searchPath[searchLen++] = '*';
            searchPath[searchLen] = '\0';
        }
    }

    hFind = FindFirstFileA(searchPath, &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        err_set(errMsg, errSize, "directory not found");
        return 0;
    }

    outPos = 0;
    first = 1;

    do {
        const char *name;
        int nameLen;
        int isDir;

        name = findData.cFileName;

        /* Skip . and .. */
        if (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }

        isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        /* Calculate bytes needed: name + optional '\' + optional '\n' + '\0' */
        nameLen = 0;
        while (name[nameLen] != '\0') {
            nameLen++;
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

        /* Entry name */
        for (i = 0; i < nameLen; i++) {
            out[outPos++] = name[i];
        }

        /* Directory suffix */
        if (isDir) {
            out[outPos++] = '\\';
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
    out[outPos] = '\0';

    if (first) {
        /* Empty directory — return success with empty output */
        return 1;
    }

    return 1;
}

int FileOpDelete(const char *path, char *errMsg, int errSize)
{
    if (path == NULL) {
        err_set(errMsg, errSize, "null parameter");
        return 0;
    }

    if (!DeleteFileA(path)) {
        err_set(errMsg, errSize, "delete failed");
        return 0;
    }

    return 1;
}
