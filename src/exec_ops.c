/*
 * exec_ops.c - Child-process spawn, capture and timeout.
 *
 * One child per call. Win32s-baseline correct, uplifting at runtime via
 * g_features. See exec_ops.h for the contract and the orphan-domain note.
 *
 * Win32 quirks designed around (PHASE4 table):
 *   Q1  WaitForSingleObject(hProcess) returns immediately on Win32s 1.25a
 *       (KB Q125213) - the polling path polls GetExitCodeProcess until it
 *       is != STILL_ACTIVE instead.
 *   Q2  A full stdin write (<= 4096, one pipe buffer) cannot block; we
 *       write all of it then close the write end.
 *   Q3  PeekNamedPipe is the only single-threaded non-blocking pipe read;
 *       PumpPipe peeks before every ReadFile.
 *   Q4  Parent closes the child's pipe ends right after CreateProcessA or
 *       the pipes never reach EOF (MS "Creating a Child Process with
 *       Redirected Input and Output").
 *   Q5  Parent-only handle ends get HANDLE_FLAG_INHERIT cleared so the
 *       child does not hold them open.
 *  Q10/Q11 STARTF_USESHOWWINDOW + SW_HIDE (never CREATE_NO_WINDOW, which
 *       is Win95+ and ignored on Win32s).
 *   Q12 16-bit (NE16/MZ) children share the VDM; on timeout we do NOT
 *       TerminateProcess - we leave them still_active.
 *   Q13 GetTickCount, not QueryPerformanceCounter (QPC is 95+ and may
 *       pull FP libs). Deltas are unsigned wrap-safe.
 *
 * Pipe deadlock avoidance (Old New Thing, 2011-07-07): the polling path
 * pumps both output pipes inside the wait loop, never only after the
 * child exits.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include "exec_ops.h"
#include "feat.h"
#include "binfmt.h"
#include "encoding.h"   /* Utf8ToUtf16 for the -W spawn (wide tier) */

/* ------------------------------------------------------------------
 * Job-object declarations. MinGW's C89 headers may lack these; declare
 * them ourselves under #ifndef guards so we can build everywhere. They
 * are only ever USED through g_features.p* pointers, so referencing the
 * structs here does not create a link-time import.
 * ------------------------------------------------------------------ */

#ifndef JOB_OBJECT_LIMIT_PROCESS_MEMORY
#define JOB_OBJECT_LIMIT_PROCESS_MEMORY    0x00000100
#endif
#ifndef JOB_OBJECT_LIMIT_PROCESS_TIME
#define JOB_OBJECT_LIMIT_PROCESS_TIME      0x00000002
#endif
#ifndef JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#endif

/* JobObjectExtendedLimitInformation class index. */
#ifndef JobObjectExtendedLimitInformation
#define JobObjectExtendedLimitInformation 9
#endif

#ifndef EXEC_OPS_HAVE_JOBINFO
typedef struct _EXEC_IO_COUNTERS {
    DWORDLONG ReadOperationCount;
    DWORDLONG WriteOperationCount;
    DWORDLONG OtherOperationCount;
    DWORDLONG ReadTransferCount;
    DWORDLONG WriteTransferCount;
    DWORDLONG OtherTransferCount;
} EXEC_IO_COUNTERS;

typedef struct _EXEC_JOBOBJECT_BASIC_LIMIT_INFORMATION {
    LARGE_INTEGER PerProcessUserTimeLimit;
    LARGE_INTEGER PerJobUserTimeLimit;
    DWORD         LimitFlags;
    SIZE_T        MinimumWorkingSetSize;
    SIZE_T        MaximumWorkingSetSize;
    DWORD         ActiveProcessLimit;
    ULONG_PTR     Affinity;
    DWORD         PriorityClass;
    DWORD         SchedulingClass;
} EXEC_JOBOBJECT_BASIC_LIMIT_INFORMATION;

typedef struct _EXEC_JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
    EXEC_JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    EXEC_IO_COUNTERS IoInfo;
    SIZE_T ProcessMemoryLimit;
    SIZE_T JobMemoryLimit;
    SIZE_T PeakProcessMemoryUsed;
    SIZE_T PeakJobMemoryUsed;
} EXEC_JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
#endif /* EXEC_OPS_HAVE_JOBINFO */

/* STILL_ACTIVE is STATUS_PENDING (259); define if absent. */
#ifndef STILL_ACTIVE
#define STILL_ACTIVE 259
#endif

/* Max command line CreateProcessA accepts is 32767 incl. the NUL; we
 * refuse anything that would not leave room (Q7 shell caps are the
 * caller's job; here we only enforce the hard CreateProcessA limit). */
#define EXEC_MAX_CMDLINE 32766

/* Largest single ReadFile chunk in the pump. */
#define EXEC_READ_CHUNK 4096

/* ------------------------------------------------------------------
 * Threaded-path reader context (Win 9x / NT+).
 * ------------------------------------------------------------------ */

typedef struct {
    HANDLE           pipe;       /* parent read end */
    unsigned char   *buf;
    int              bufSize;
    int              len;        /* bytes captured so far */
    int              truncated;  /* 1 once the buffer fills */
    CRITICAL_SECTION *lock;
} ReaderCtx;

/*
 * SetMsg - copy a short error message into the caller's buffer.
 */
static void SetMsg(char *errMsg, int errSize, const char *s)
{
    if (errMsg != NULL && errSize > 0) {
        lstrcpynA(errMsg, s, errSize);
    }
}

/*
 * PumpPipe - polling-path non-blocking drain (Q3). PeekNamedPipe to find
 * how many bytes are available; ReadFile up to the smaller of that and
 * the remaining buffer. Sets *truncated when bytes are available but the
 * buffer is full.
 */
static void PumpPipe(HANDLE pipe, unsigned char *buf, int *len, int bufSize,
                     int *truncated)
{
    DWORD avail;
    DWORD toRead;
    DWORD got;
    int   remaining;

    for (;;) {
        avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
            return;     /* pipe closed/broken */
        }
        if (avail == 0) {
            return;
        }

        remaining = bufSize - *len;
        if (remaining <= 0) {
            *truncated = 1;
            return;     /* full; leave the rest in the pipe */
        }

        toRead = avail;
        if (toRead > (DWORD)remaining) {
            toRead = (DWORD)remaining;
        }
        if (toRead > EXEC_READ_CHUNK) {
            toRead = EXEC_READ_CHUNK;
        }

        got = 0;
        if (!ReadFile(pipe, buf + *len, toRead, &got, NULL) || got == 0) {
            return;
        }
        *len += (int)got;
        if (avail > got && *len >= bufSize) {
            *truncated = 1;
        }
    }
}

/*
 * ReaderThread - threaded-path drain. Blocking ReadFile loop until the
 * pipe EOFs (child closed its write end) or errors; appends under the
 * shared CRITICAL_SECTION, marking truncated once the buffer fills.
 */
static DWORD WINAPI ReaderThread(LPVOID param)
{
    ReaderCtx *c = (ReaderCtx *)param;
    unsigned char chunk[EXEC_READ_CHUNK];
    DWORD got;
    int   remaining;
    int   copy;

    for (;;) {
        got = 0;
        if (!ReadFile(c->pipe, chunk, sizeof(chunk), &got, NULL) || got == 0) {
            break;
        }
        EnterCriticalSection(c->lock);
        remaining = c->bufSize - c->len;
        if (remaining <= 0) {
            c->truncated = 1;
        } else {
            copy = (int)got;
            if (copy > remaining) {
                copy = remaining;
                c->truncated = 1;
            }
            memcpy(c->buf + c->len, chunk, (size_t)copy);
            c->len += copy;
        }
        LeaveCriticalSection(c->lock);
    }
    return 0;
}

/*
 * TimeoutTerminate - end (or deliberately not end) a child that ran past
 * its timeout. PHASE4 TimeoutTerminate, verbatim in intent:
 *   - 16-bit (NE16/MZ): never kill (Q12 shared VDM). Caller turns this
 *     into the still_active orphan; killedBy stays NONE here and the
 *     orphan flag is signalled via the return value.
 *   - else, with GenerateConsoleCtrlEvent (NT 4.0+, child in its own
 *     process group): CTRL_BREAK_EVENT, wait up to 1s; if it exits,
 *     killed_by = ctrl_break.
 *   - otherwise TerminateProcess, killed_by = timeout.
 *
 * Returns 1 if the child is still alive and was deliberately left so
 * (the 16-bit orphan case); 0 if it was killed (or already gone).
 */
static int TimeoutTerminate(HANDLE hProc, DWORD procGroupId, int binaryType,
                            int *killedBy)
{
    if (binaryType == BIN_NE16 || binaryType == BIN_MZ) {
        /* Q12: do not kill the shared VDM. Left still_active. */
        *killedBy = EXEC_KILLED_NONE;
        return 1;
    }

    if (g_features.has_generate_ctrl_event &&
        g_features.pGenerateConsoleCtrlEvent != NULL) {
        if (g_features.pGenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                                 procGroupId)) {
            if (WaitForSingleObject(hProc, 1000) == WAIT_OBJECT_0) {
                *killedBy = EXEC_KILLED_CTRL_BREAK;
                return 0;
            }
        }
    }

    TerminateProcess(hProc, 1);
    *killedBy = EXEC_KILLED_TIMEOUT;
    return 0;
}

/*
 * SetUpJob - create a job object, apply the requested limits, assign the
 * (suspended) child, and resume it. Only called when
 * g_features.has_create_job_object. On any failure returns 0 (caller
 * terminates the child and reports "job object setup failed"); on
 * success *outJob receives the handle to close after the child exits.
 */
static int SetUpJob(HANDLE hProc, HANDLE hThread, int memCapBytes,
                    int cpuTimeMs, HANDLE *outJob)
{
    EXEC_JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    HANDLE hJob;

    *outJob = NULL;
    hJob = g_features.pCreateJobObjectA(NULL, NULL);
    if (hJob == NULL) {
        return 0;
    }

    memset(&info, 0, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (memCapBytes > 0) {
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        info.ProcessMemoryLimit = (SIZE_T)memCapBytes;
    }
    if (cpuTimeMs > 0) {
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
        /* 100ns ticks: cpuTimeMs * 10000. */
        info.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
            (LONGLONG)cpuTimeMs * 10000;
    }

    if (!g_features.pSetInformationJobObject(hJob,
            JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(hJob);
        return 0;
    }
    if (!g_features.pAssignProcessToJobObject(hJob, hProc)) {
        CloseHandle(hJob);
        return 0;
    }
    ResumeThread(hThread);
    *outJob = hJob;
    return 1;
}

int ExecOpRun(
    const char *cmdLine,
    const char *cwd,
    int  timeoutMs,
    int  hideWindow,
    const unsigned char *stdinBytes,
    int  stdinLen,
    unsigned char *stdoutBuf, int stdoutBufSize,
    unsigned char *stderrBuf, int stderrBufSize,
    int  memCapBytes,
    int  cpuTimeMs,
    int  binaryType,
    ExecResult *result,
    char *errMsg, int errSize)
{
    /* All declarations at block top (C89). Large structs kept on the
     * stack are small enough to avoid __chkstk; the capture buffers are
     * the caller's. */
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE inRd, inWr, outRd, outWr, errRd, errWr;
    HANDLE hJob;
    HANDLE hOutThread, hErrThread;
    CRITICAL_SECTION lock;
    ReaderCtx outCtx, errCtx;
    DWORD creationFlags;
    DWORD exitCode;
    DWORD startTick, elapsed;
    DWORD wantCaps;
    int   stdoutLen, stderrLen, stdoutTrunc, stderrTrunc;
    int   killedBy;
    int   leftOrphan;
    int   spawnOk;
    int   useThreads;

    /* Initialise result defensively. */
    result->exit_code = -1;
    result->duration_ms = 0;
    result->stdout_len = 0;
    result->stderr_len = 0;
    result->stdout_truncated = 0;
    result->stderr_truncated = 0;
    result->timed_out = 0;
    result->killed_by = EXEC_KILLED_NONE;
    result->still_active = 0;
    result->orphan_handle = NULL;
    result->orphan_start_tick = 0;

    inRd = inWr = outRd = outWr = errRd = errWr = NULL;
    hJob = NULL;
    hOutThread = hErrThread = NULL;
    stdoutLen = stderrLen = stdoutTrunc = stderrTrunc = 0;
    killedBy = EXEC_KILLED_NONE;
    leftOrphan = 0;
    wantCaps = (DWORD)(memCapBytes > 0 || cpuTimeMs > 0);

    if (cmdLine == NULL || cmdLine[0] == '\0') {
        SetMsg(errMsg, errSize, "spawn failed: empty command line");
        return 0;
    }
    if (lstrlenA(cmdLine) > EXEC_MAX_CMDLINE) {
        SetMsg(errMsg, errSize, "command line too long");
        return 0;
    }

    /* Pipes inherit by default; parent-only ends get inherit cleared. */
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&inRd, &inWr, &sa, 0) ||
        !CreatePipe(&outRd, &outWr, &sa, 0) ||
        !CreatePipe(&errRd, &errWr, &sa, 0)) {
        SetMsg(errMsg, errSize, "pipe creation failed");
        goto fail_pipes;
    }
    /* Q5: child must not inherit the parent-only ends. */
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = errWr;
    if (hideWindow) {
        si.dwFlags |= STARTF_USESHOWWINDOW;     /* Q10/Q11 */
        si.wShowWindow = SW_HIDE;
    }

    creationFlags = 0;
    /* Own process group lets us GenerateConsoleCtrlEvent later. */
    if (g_features.has_generate_ctrl_event) {
        creationFlags |= CREATE_NEW_PROCESS_GROUP;
    }
    /* Suspend so the job can be assigned before the first instruction. */
    if (g_features.has_create_job_object) {
        creationFlags |= CREATE_SUSPENDED;
    }

    memset(&pi, 0, sizeof(pi));

    /* Children inherit the spawner's error mode: suppress hard-error
     * dialogs (e.g. the loader's "unable to start correctly"
     * 0xC000012D box when a job memory cap starves the child's
     * startup commit). A blocking dialog on a hidden child would hang
     * the capture loop until someone clicks OK on the server's
     * desktop. Restored immediately after the spawn snapshot. */
    {
        UINT oldErrMode;
        oldErrMode = SetErrorMode(SEM_FAILCRITICALERRORS |
                                  SEM_NOGPFAULTERRORBOX |
                                  SEM_NOOPENFILEERRORBOX);
        if (g_features.has_wide_createprocess) {
            /* Wide tier (NT family): the cmdLine + cwd are UTF-8; widen them
             * and spawn through CreateProcessW so non-ASCII argv survives
             * losslessly. lpCommandLine must be writable. The job/orphan
             * machinery below is HANDLE-based and unchanged. The single
             * threaded server makes the 64 KB widen buffer safe as static. */
            static WCHAR wCmd[EXEC_MAX_CMDLINE + 2];
            static WCHAR wCwd[MAX_PATH];
            STARTUPINFOW siw;
            EncStatus est;
            LPCWSTR wCwdArg;
            int nw;

            nw = Utf8ToUtf16((const unsigned char *)cmdLine, lstrlenA(cmdLine),
                             (unsigned short *)wCmd, EXEC_MAX_CMDLINE, &est);
            wCmd[nw] = 0;

            wCwdArg = NULL;
            if (cwd != NULL && cwd[0] != '\0') {
                int ncw;
                ncw = Utf8ToUtf16((const unsigned char *)cwd, lstrlenA(cwd),
                                  (unsigned short *)wCwd, MAX_PATH - 1, &est);
                wCwd[ncw] = 0;
                wCwdArg = wCwd;
            }

            memset(&siw, 0, sizeof(siw));
            siw.cb = sizeof(siw);
            siw.dwFlags = si.dwFlags;
            siw.hStdInput = si.hStdInput;
            siw.hStdOutput = si.hStdOutput;
            siw.hStdError = si.hStdError;
            siw.wShowWindow = si.wShowWindow;

            spawnOk = g_features.pCreateProcessW(NULL, wCmd, NULL, NULL, TRUE,
                                                 creationFlags, NULL, wCwdArg,
                                                 &siw, &pi);
        } else {
            /* -A fallback (Win32s/9x, or the uplift forced off): pass the
             * bytes through as today. */
            spawnOk = CreateProcessA(NULL, (LPSTR)cmdLine, NULL, NULL, TRUE,
                                     creationFlags, NULL, cwd, &si, &pi);
        }
        SetErrorMode(oldErrMode);
    }
    if (!spawnOk) {
        char buf[64];
        wsprintfA(buf, "spawn failed: %lu", (unsigned long)GetLastError());
        SetMsg(errMsg, errSize, buf);
        goto fail_spawn;
    }

    startTick = GetTickCount();

    /* Job-object containment / resource caps (NT 4.0+). */
    if (g_features.has_create_job_object) {
        if (!SetUpJob(pi.hProcess, pi.hThread, memCapBytes, cpuTimeMs,
                      &hJob)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetMsg(errMsg, errSize, "job object setup failed");
            goto fail_spawn;
        }
        /* SetUpJob resumed the child. */
    }

    /* Q4: close the child's pipe ends in the parent so the output pipes
     * can reach EOF and the child's stdin read sees EOF after our write. */
    CloseHandle(inRd);  inRd = NULL;
    CloseHandle(outWr); outWr = NULL;
    CloseHandle(errWr); errWr = NULL;

    /* Q2: one full (<= 4096) stdin write cannot block. Then close the
     * write end so the child's stdin reaches EOF. */
    if (stdinBytes != NULL && stdinLen > 0) {
        DWORD wrote = 0;
        WriteFile(inWr, stdinBytes, (DWORD)stdinLen, &wrote, NULL);
    }
    CloseHandle(inWr); inWr = NULL;

    /* Potential orphans (16-bit children, Q12) ALWAYS use the polling
     * capture even on threaded hosts: if the child is left still_active,
     * reader threads blocked in ReadFile on its live pipes cannot be
     * cancelled portably (closing the read end does not unblock a
     * blocked anonymous-pipe ReadFile), so the join would hang. The
     * non-blocking PeekNamedPipe loop exits cleanly around an orphan. */
    useThreads = (g_features.has_threads != 0 &&
                  binaryType != (int)BIN_NE16 &&
                  binaryType != (int)BIN_MZ);

    if (!useThreads) {
        /* ---- Win32s / Win9x polling path (Q1, Q3). ---- */
        for (;;) {
            PumpPipe(outRd, stdoutBuf, &stdoutLen, stdoutBufSize, &stdoutTrunc);
            PumpPipe(errRd, stderrBuf, &stderrLen, stderrBufSize, &stderrTrunc);

            exitCode = STILL_ACTIVE;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            if (exitCode != STILL_ACTIVE) {
                break;                          /* Q1 */
            }

            elapsed = GetTickCount() - startTick;   /* wrap-safe */
            if ((DWORD)timeoutMs > 0 && elapsed >= (DWORD)timeoutMs) {
                leftOrphan = TimeoutTerminate(pi.hProcess, pi.dwProcessId,
                                              binaryType, &killedBy);
                break;
            }
            Sleep(10);
        }
        /* Final drain. */
        PumpPipe(outRd, stdoutBuf, &stdoutLen, stdoutBufSize, &stdoutTrunc);
        PumpPipe(errRd, stderrBuf, &stderrLen, stderrBufSize, &stderrTrunc);
    } else {
        /* ---- Threaded path (Win 9x / NT+). ---- */
        DWORD wait;

        InitializeCriticalSection(&lock);
        outCtx.pipe = outRd; outCtx.buf = stdoutBuf;
        outCtx.bufSize = stdoutBufSize; outCtx.len = 0;
        outCtx.truncated = 0; outCtx.lock = &lock;
        errCtx.pipe = errRd; errCtx.buf = stderrBuf;
        errCtx.bufSize = stderrBufSize; errCtx.len = 0;
        errCtx.truncated = 0; errCtx.lock = &lock;

        hOutThread = CreateThread(NULL, 0, ReaderThread, &outCtx, 0, NULL);
        hErrThread = CreateThread(NULL, 0, ReaderThread, &errCtx, 0, NULL);

        wait = WaitForSingleObject(pi.hProcess,
                                   (DWORD)timeoutMs > 0 ? (DWORD)timeoutMs
                                                        : INFINITE);
        if (wait == WAIT_TIMEOUT) {
            leftOrphan = TimeoutTerminate(pi.hProcess, pi.dwProcessId,
                                          binaryType, &killedBy);
            if (!leftOrphan) {
                WaitForSingleObject(pi.hProcess, INFINITE);
            }
        }

        /* Reader threads exit when their pipe EOFs after the child dies.
         * For the 16-bit orphan the child is still alive, so its write
         * ends are still open; close our read ends to break the readers
         * out, then they unblock on the resulting error. */
        if (leftOrphan) {
            CloseHandle(outRd); outRd = NULL;
            CloseHandle(errRd); errRd = NULL;
        }
        if (hOutThread != NULL) {
            WaitForSingleObject(hOutThread, INFINITE);
            CloseHandle(hOutThread);
            hOutThread = NULL;
        }
        if (hErrThread != NULL) {
            WaitForSingleObject(hErrThread, INFINITE);
            CloseHandle(hErrThread);
            hErrThread = NULL;
        }

        stdoutLen = outCtx.len; stdoutTrunc = outCtx.truncated;
        stderrLen = errCtx.len; stderrTrunc = errCtx.truncated;
        DeleteCriticalSection(&lock);
    }

    elapsed = GetTickCount() - startTick;       /* Q13, wrap-safe */

    if (leftOrphan) {
        /* 16-bit timeout: child deliberately left running (Q12). The
         * dispatcher owns the handle and reaps it later; do NOT close it
         * and do NOT close the job (closing a kill-on-close job would
         * kill the orphan). */
        result->still_active = 1;
        result->orphan_handle = pi.hProcess;
        result->orphan_start_tick = startTick;
        result->exit_code = STILL_ACTIVE;
        result->timed_out = 0;          /* not killed => not timed_out */
        result->killed_by = EXEC_KILLED_NONE;
        CloseHandle(pi.hThread);
        if (outRd != NULL) CloseHandle(outRd);
        if (errRd != NULL) CloseHandle(errRd);
        /* hJob is never set for the orphan path: 16-bit children are a
         * Win32s/VDM concern and job objects are an NT 4.0+ feature; on
         * NT the manual VDM-no-kill rule still governs by binary type.
         * If a job were present, leaving it open keeps kill-on-close
         * from firing until the dispatcher reaps. */
        result->stdout_len = stdoutLen;
        result->stderr_len = stderrLen;
        result->stdout_truncated = stdoutTrunc;
        result->stderr_truncated = stderrTrunc;
        result->duration_ms = (int)elapsed;
        return 1;
    }

    /* Child is gone: read its exit code. */
    exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    /* Job-limit kill detection (heuristic, documented). Windows job
     * semantics differ per limit: JOB_OBJECT_LIMIT_PROCESS_TIME
     * TERMINATES the child at the cap (exit code 1816,
     * ERROR_NOT_ENOUGH_QUOTA), while JOB_OBJECT_LIMIT_PROCESS_MEMORY
     * only FAILS further allocations - the child is never killed by the
     * kernel, it fails on its own. So cpu_cap is attributable when a
     * CPU-capped child died abnormally without our timeout; memory_cap
     * is attributed only via the documented quota exit code, which a
     * child killed by commit failure typically does not produce - the
     * memory cap usually surfaces as the child's own nonzero exit with
     * killed_by none. */
    if (killedBy == EXEC_KILLED_NONE && hJob != NULL && wantCaps &&
        exitCode != 0) {
        if (cpuTimeMs > 0 && exitCode == 1816UL) {
            killedBy = EXEC_KILLED_CPU_CAP;
        } else if (cpuTimeMs > 0 && memCapBytes == 0) {
            killedBy = EXEC_KILLED_CPU_CAP;
        } else if (memCapBytes > 0 && exitCode == 1816UL) {
            killedBy = EXEC_KILLED_MEMORY_CAP;
        }
    }

    result->exit_code = (int)exitCode;
    result->duration_ms = (int)elapsed;
    result->stdout_len = stdoutLen;
    result->stderr_len = stderrLen;
    result->stdout_truncated = stdoutTrunc;
    result->stderr_truncated = stderrTrunc;
    result->killed_by = killedBy;
    result->timed_out = (killedBy == EXEC_KILLED_TIMEOUT ||
                         killedBy == EXEC_KILLED_CTRL_BREAK) ? 1 : 0;

    /* Universal cleanup. Closing a kill-on-close job here is fine: the
     * child already exited. */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hJob != NULL) CloseHandle(hJob);
    if (outRd != NULL) CloseHandle(outRd);
    if (errRd != NULL) CloseHandle(errRd);
    return 1;

fail_spawn:
    /* Pipes only; child (if any) already handled above. */
    if (inRd != NULL)  CloseHandle(inRd);
    if (inWr != NULL)  CloseHandle(inWr);
    if (outRd != NULL) CloseHandle(outRd);
    if (outWr != NULL) CloseHandle(outWr);
    if (errRd != NULL) CloseHandle(errRd);
    if (errWr != NULL) CloseHandle(errWr);
    return 0;

fail_pipes:
    if (inRd != NULL)  CloseHandle(inRd);
    if (inWr != NULL)  CloseHandle(inWr);
    if (outRd != NULL) CloseHandle(outRd);
    if (outWr != NULL) CloseHandle(outWr);
    if (errRd != NULL) CloseHandle(errRd);
    if (errWr != NULL) CloseHandle(errWr);
    return 0;
}
