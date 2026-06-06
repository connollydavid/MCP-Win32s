/*
 * exec_ops.h - Child-process spawn, capture and timeout for one exec
 *
 * exec_ops owns the full lifecycle of ONE child process: build the
 * stdin/stdout/stderr pipes, CreateProcessA, pump the pipes while the
 * child runs, enforce the timeout, and report the result. It is the
 * Win32s-baseline-correct core that uplifts at runtime via g_features:
 *
 *   - Capture loop: PeekNamedPipe polling on Win32s (no threads), two
 *     reader threads guarded by a CRITICAL_SECTION on Win 9x/NT+.
 *   - Containment: per-child job object (kill-on-close + optional memory
 *     and CPU-time caps) on NT 4.0+, silently skipped where absent.
 *   - Termination: graceful CTRL_BREAK_EVENT to the child's process
 *     group on NT 4.0+ before falling through to TerminateProcess.
 *
 * The still_active / busy ORPHAN DOMAIN itself is dispatcher state
 * (mcp-w32s.c), not owned here. But exec_ops SUPPORTS it: when a 16-bit
 * (BIN_NE16 / BIN_MZ) child times out it is NOT killed (Q12, shared VDM)
 * and its process handle is NOT closed - it is handed back to the caller
 * in ExecResult.orphan_handle with ExecResult.still_active = 1, so the
 * dispatcher can re-poll GetExitCodeProcess on later requests and reap it
 * (implicit reap with informative busy). In every other case the process
 * handle is closed before ExecOpRun returns.
 *
 * Sources cited in exec_ops.c:
 *   KB Q125213  - Win32s WaitForSingleObject(hProcess) returns
 *                 immediately; poll GetExitCodeProcess (Q1).
 *   Old New Thing 2011-07-07 - pipe deadlock; pump inside the wait loop.
 *   MS Docs "Creating a Child Process with Redirected Input and Output"
 *                 - pipe/handle-inheritance setup (Q4/Q5).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef EXEC_OPS_H
#define EXEC_OPS_H

#include <windows.h>

/* killed_by codes (mirror the spec's KilledBy enum and the JSON
 * vocabulary none|timeout|ctrl_break|memory_cap|cpu_cap). */
#define EXEC_KILLED_NONE        0
#define EXEC_KILLED_TIMEOUT     1
#define EXEC_KILLED_CTRL_BREAK  2
#define EXEC_KILLED_MEMORY_CAP  3
#define EXEC_KILLED_CPU_CAP     4

typedef struct {
    int    exit_code;          /* child exit code; -1 if spawn failed */
    int    duration_ms;        /* GetTickCount delta, wrap-safe (Q13) */
    int    stdout_len;
    int    stderr_len;
    int    stdout_truncated;   /* 1 if stdout filled its buffer */
    int    stderr_truncated;   /* 1 if stderr filled its buffer */
    int    timed_out;          /* 1 iff the child was KILLED on timeout */
    int    killed_by;          /* EXEC_KILLED_* */

    /* Orphan domain support (16-bit timeout, Q12). When still_active is
     * 1 the child was deliberately left running and its process handle is
     * returned in orphan_handle (NOT closed by ExecOpRun); the caller
     * owns it and must CloseHandle it after reaping. orphan_start_tick is
     * the GetTickCount value at spawn, for the busy elapsed-ms report.
     * In all other cases still_active = 0 and orphan_handle = NULL. */
    int    still_active;
    HANDLE orphan_handle;
    DWORD  orphan_start_tick;
} ExecResult;

/*
 * ExecOpRun - Spawn one child, capture its output, enforce the timeout.
 *
 *   cmdLine       full command line (lpApplicationName is always NULL so
 *                 Windows resolves via PATH, Q14). Refused with "command
 *                 line too long" when longer than 32766 chars. Length
 *                 caps for the shell case (Q7) are the caller's job.
 *   cwd           working directory, or NULL to inherit the parent's.
 *   timeoutMs     resolved by the caller; always > 0.
 *   hideWindow    nonzero => STARTF_USESHOWWINDOW + SW_HIDE (Q10/Q11).
 *   stdinBytes    bytes to write to the child's stdin, or NULL.
 *   stdinLen      number of stdin bytes (caller guarantees <= 4096, one
 *                 pipe buffer, so the single write can never block - Q2).
 *   stdoutBuf/Size, stderrBuf/Size  capture buffers.
 *   memCapBytes   >0 => per-process memory cap via job object (NT 4.0+).
 *   cpuTimeMs     >0 => per-process CPU-time cap via job object.
 *   binaryType    BinaryType from binfmt (caller-classified). BIN_NE16 /
 *                 BIN_MZ select the no-kill VDM timeout path.
 *   result        out: populated on success AND on in-band failure
 *                 (timeout / still_active).
 *   errMsg/Size   out: short message on hard failure (return 0).
 *
 * Returns 1 when the child ran to a reportable outcome (exited, killed on
 * timeout, or left still_active); result is filled. Returns 0 on a hard
 * failure that produced no Process (spawn failed, command line too long,
 * pipe creation failed); errMsg is set and result->exit_code = -1.
 */
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
    char *errMsg, int errSize
);

#endif /* EXEC_OPS_H */
