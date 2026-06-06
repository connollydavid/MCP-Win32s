/*
 * pty_exec.c - Pseudoconsole (PTY) child execution for MCP-Win32s
 *
 * See pty_exec.h. Spawns a child under a Win10 1809+ pseudo console and
 * captures its single merged ANSI stream. All pseudoconsole and
 * attribute-list calls go through g_features.p* pointers (feat.h); the
 * capability is gated on g_features.has_create_pseudo_console.
 *
 * Capture uses a reader thread: a pseudo console implies Win10, which
 * always has threads, so CreateThread (a legal static import that merely
 * fails on Win32s) is used directly. The main thread waits on the child
 * with WaitForSingleObject (correct outside Win32s; Q1 only affects
 * Win32s). On timeout the child is TerminateProcess'd - there is no VDM
 * concern on Win10 (Q12). Closing the pseudo console (pClosePseudoConsole)
 * EOFs the output pipe, which lets the reader thread exit so its handle
 * can be joined.
 *
 * Sources: MS Docs "Creating a Pseudoconsole Session"; KB Q125213
 * (Win32s synchronous spawn, not applicable here); MS Docs
 * "Creating a Child Process with Redirected Input and Output".
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdlib.h>
#include "pty_exec.h"
#include "feat.h"

/*
 * EXTENDED_STARTUPINFO_PRESENT is supplied by current MinGW headers but
 * PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE is not (C89 SDK subset). Guard both
 * so the module builds regardless of header vintage.
 */
#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

/*
 * StartupInfoExA - STARTUPINFOEXA equivalent. The old SDK headers lack
 * the EX struct (it needs the attribute-list typedef), so declare a
 * local copy: a STARTUPINFOA followed by the opaque attribute-list
 * pointer. Layout matches the Win32 STARTUPINFOEXA exactly.
 */
typedef struct {
    STARTUPINFOA StartupInfo;
    void *lpAttributeList;
} StartupInfoExA;

/*
 * ReaderContext - shared state for the output-pipe reader thread. The
 * reader appends bytes until the pipe EOFs (which happens once the child
 * exits and the pseudo console is closed) or the buffer fills.
 */
typedef struct {
    HANDLE hPipe;
    unsigned char *buf;
    int bufSize;
    int len;
    int truncated;
} ReaderContext;

/*
 * copy_str - Bounded string copy (errMsg helper).
 */
static void copy_str(char *dst, int dstSize, const char *src)
{
    int i;
    if (dst == 0 || dstSize <= 0) {
        return;
    }
    for (i = 0; i < dstSize - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * ReaderThread - drain the output pipe into the caller's buffer. Loops
 * ReadFile until 0 bytes or an error (the broken-pipe error after the
 * write end closes counts as EOF). Marks truncated when the buffer fills.
 */
static DWORD WINAPI ReaderThread(LPVOID lpParam)
{
    ReaderContext *ctx;
    unsigned char chunk[1024];
    DWORD got;
    int room;
    int copy;

    ctx = (ReaderContext *)lpParam;
    for (;;) {
        if (!ReadFile(ctx->hPipe, chunk, sizeof(chunk), &got, NULL)) {
            break;
        }
        if (got == 0) {
            break;
        }
        room = ctx->bufSize - ctx->len;
        copy = (int)got;
        if (copy > room) {
            copy = room;
            ctx->truncated = 1;
        }
        if (copy > 0) {
            CopyMemory(ctx->buf + ctx->len, chunk, copy);
            ctx->len += copy;
        }
    }
    return 0;
}

int PtyExecRun(
    const char *cmdLine,
    const char *cwd,
    int cols, int rows,
    int timeoutMs,
    const unsigned char *stdinBytes, int stdinLen,
    unsigned char *outputBuf, int outputBufSize,
    PtyExecResult *result,
    char *errMsg, int errSize)
{
    HANDLE hInputRd;
    HANDLE hInputWr;
    HANDLE hOutputRd;
    HANDLE hOutputWr;
    void *hPC;
    void *attrList;
    StartupInfoExA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    ReaderContext rctx;
    HANDLE hReader;
    COORD size;
    FeatSizeT attrSize;
    DWORD start;
    DWORD exitCode;
    DWORD wait;
    LONG hr;
    char *mutableCmd;
    int cmdLen;

    /* Initialise outputs first so every early return is well-defined. */
    if (errMsg != 0 && errSize > 0) {
        errMsg[0] = '\0';
    }
    result->exit_code = -1;
    result->duration_ms = 0;
    result->output_len = 0;
    result->output_truncated = 0;
    result->timed_out = 0;

    hInputRd = INVALID_HANDLE_VALUE;
    hInputWr = INVALID_HANDLE_VALUE;
    hOutputRd = INVALID_HANDLE_VALUE;
    hOutputWr = INVALID_HANDLE_VALUE;
    hPC = NULL;
    attrList = NULL;
    hReader = NULL;
    mutableCmd = NULL;
    ZeroMemory(&pi, sizeof(pi));

    /* Capability gate (process-ops.allium rule PtyUnavailable). */
    if (!g_features.has_create_pseudo_console) {
        copy_str(errMsg, errSize, "pty not available on this Windows");
        return 0;
    }

    /* Two pipes: child input and child output. The pseudo console merges
       stdout and stderr by design, so a single output pipe suffices. */
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hInputRd, &hInputWr, &sa, 0)) {
        copy_str(errMsg, errSize, "pipe creation failed");
        return 0;
    }
    if (!CreatePipe(&hOutputRd, &hOutputWr, &sa, 0)) {
        copy_str(errMsg, errSize, "pipe creation failed");
        CloseHandle(hInputRd);
        CloseHandle(hInputWr);
        return 0;
    }

    /* Create the pseudo console over the child-side pipe ends. */
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    hr = g_features.pCreatePseudoConsole(size, hInputRd, hOutputWr, 0, &hPC);
    if (hr != 0) {            /* S_OK == 0 */
        copy_str(errMsg, errSize, "pseudo console creation failed");
        goto cleanup;
    }

    /* The pseudo console now owns the child-side ends; close our copies
       so the only remaining references are the parent-side ends. */
    CloseHandle(hInputRd);
    hInputRd = INVALID_HANDLE_VALUE;
    CloseHandle(hOutputWr);
    hOutputWr = INVALID_HANDLE_VALUE;

    /* Size, allocate and initialise the proc-thread attribute list. */
    attrSize = 0;
    g_features.pInitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    if (attrSize == 0) {
        copy_str(errMsg, errSize, "attribute list sizing failed");
        goto cleanup;
    }
    attrList = malloc((size_t)attrSize);
    if (attrList == NULL) {
        copy_str(errMsg, errSize, "out of memory");
        goto cleanup;
    }
    if (!g_features.pInitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        copy_str(errMsg, errSize, "attribute list init failed");
        free(attrList);
        attrList = NULL;
        goto cleanup;
    }
    if (!g_features.pUpdateProcThreadAttribute(
            attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            hPC, sizeof(hPC), NULL, NULL)) {
        copy_str(errMsg, errSize, "attribute list update failed");
        goto cleanup;
    }

    /* Build the extended startup info referencing the attribute list. */
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrList;

    /* CreateProcessA may write to lpCommandLine, so pass a mutable copy. */
    cmdLen = lstrlen(cmdLine);
    mutableCmd = malloc((size_t)cmdLen + 1);
    if (mutableCmd == NULL) {
        copy_str(errMsg, errSize, "out of memory");
        goto cleanup;
    }
    lstrcpy(mutableCmd, cmdLine);

    /* Suppress inherited hard-error dialogs (see exec_ops.c): a hidden
     * child popping a blocking loader/hard-error box would hang the
     * capture until dismissed on the server's desktop. */
    {
        UINT oldErrMode;
        BOOL spawned;
        oldErrMode = SetErrorMode(SEM_FAILCRITICALERRORS |
                                  SEM_NOGPFAULTERRORBOX |
                                  SEM_NOOPENFILEERRORBOX);
        spawned = CreateProcessA(
            NULL,                       /* lpApplicationName (Q14) */
            mutableCmd,
            NULL, NULL,
            TRUE,                       /* required: child inherits the PTY */
            EXTENDED_STARTUPINFO_PRESENT,
            NULL,
            cwd,                        /* NULL inherits */
            &si.StartupInfo,
            &pi);
        SetErrorMode(oldErrMode);
        if (!spawned) {
            copy_str(errMsg, errSize, "spawn failed");
            goto cleanup;
        }
    }

    /* The child holds the only remaining references to the pipe ends it
       needs; we no longer require the parent's read end of input nor will
       we read the child input. We DO keep hOutputRd (we read it) and
       hInputWr (we write stdin to it). */

    /* Feed stdin. The input write end is left open until after the child
       exits: closing it EOFs the pseudo console's input, which conhost
       reports to the attached process as a Ctrl-C / close (exit code
       0xC000013A). The child terminates on its own ("exit") or on the
       supplied input ("exit\r\n"), so no EOF is needed to drive it. */
    if (stdinBytes != NULL && stdinLen > 0) {
        DWORD written;
        WriteFile(hInputWr, stdinBytes, (DWORD)stdinLen, &written, NULL);
    }

    /* Start the output reader thread (Win10 => threads present). */
    rctx.hPipe = hOutputRd;
    rctx.buf = outputBuf;
    rctx.bufSize = outputBufSize;
    rctx.len = 0;
    rctx.truncated = 0;
    hReader = CreateThread(NULL, 0, ReaderThread, &rctx, 0, NULL);
    if (hReader == NULL) {
        copy_str(errMsg, errSize, "reader thread creation failed");
        TerminateProcess(pi.hProcess, 1);
        goto cleanup;
    }

    /* Wait for the child, with a timeout deadline. */
    start = GetTickCount();
    wait = WaitForSingleObject(pi.hProcess,
                               timeoutMs > 0 ? (DWORD)timeoutMs : INFINITE);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        result->timed_out = 1;
    }

    /* Closing the pseudo console EOFs the output pipe so the reader can
       finish; do it before joining the reader thread. */
    g_features.pClosePseudoConsole(hPC);
    hPC = NULL;

    WaitForSingleObject(hReader, INFINITE);

    /* Wrap-safe unsigned subtraction for duration (Q13, no FP). */
    result->duration_ms = (int)(GetTickCount() - start);
    result->output_len = rctx.len;
    result->output_truncated = rctx.truncated;

    if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
        result->exit_code = (int)exitCode;
    }

    /* Success path falls through to cleanup with all handles to close. */
    if (hReader != NULL) {
        CloseHandle(hReader);
        hReader = NULL;
    }
    if (attrList != NULL) {
        g_features.pDeleteProcThreadAttributeList(attrList);
        free(attrList);
        attrList = NULL;
    }
    if (mutableCmd != NULL) {
        free(mutableCmd);
        mutableCmd = NULL;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hInputWr != INVALID_HANDLE_VALUE) {
        CloseHandle(hInputWr);
    }
    if (hOutputRd != INVALID_HANDLE_VALUE) {
        CloseHandle(hOutputRd);
    }
    return 1;

cleanup:
    /* Failure path: release whatever was acquired, every handle once. */
    if (hReader != NULL) {
        CloseHandle(hReader);
    }
    if (hPC != NULL) {
        g_features.pClosePseudoConsole(hPC);
    }
    if (attrList != NULL) {
        g_features.pDeleteProcThreadAttributeList(attrList);
        free(attrList);
    }
    if (mutableCmd != NULL) {
        free(mutableCmd);
    }
    if (pi.hProcess != NULL) {
        CloseHandle(pi.hProcess);
    }
    if (pi.hThread != NULL) {
        CloseHandle(pi.hThread);
    }
    if (hInputRd != INVALID_HANDLE_VALUE) {
        CloseHandle(hInputRd);
    }
    if (hInputWr != INVALID_HANDLE_VALUE) {
        CloseHandle(hInputWr);
    }
    if (hOutputRd != INVALID_HANDLE_VALUE) {
        CloseHandle(hOutputRd);
    }
    if (hOutputWr != INVALID_HANDLE_VALUE) {
        CloseHandle(hOutputWr);
    }
    return 0;
}
