/*
 * pty_exec.h - Pseudoconsole (PTY) child execution for MCP-Win32s
 *
 * Implements the ptyExec command's spawn/capture path on Windows 10
 * 1809+ via CreatePseudoConsole. The pseudo console merges stdout and
 * stderr into a single ANSI byte stream (output_kind "ansi"), supports
 * interactive stdin, and carries cols/rows sizing.
 *
 * Every pseudoconsole and attribute-list API is invoked through the
 * g_features.p* function pointers (feat.h) - never by name - so the
 * binary still loads on Win32s 1.25a where these symbols are absent.
 * When g_features.has_create_pseudo_console is 0, PtyExecRun returns 0
 * with errMsg "pty not available on this Windows" (process-ops.allium
 * rule PtyUnavailable).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef PTY_EXEC_H
#define PTY_EXEC_H

/*
 * PtyExecResult - Outcome of a ptyExec run. Mirrors the PtyExecResult
 * entity in specs/process-ops.allium (single merged output stream).
 */
typedef struct {
    int exit_code;          /* child exit code; -1 if spawn failed */
    int duration_ms;        /* wall time, GetTickCount delta */
    int output_len;         /* bytes captured into outputBuf */
    int output_truncated;   /* 1 if outputBuf filled before EOF */
    int timed_out;          /* 1 if killed on timeout */
} PtyExecResult;

/*
 * PtyExecRun - Spawn cmdLine under a pseudo console and capture its
 * merged ANSI output.
 *
 * Returns 1 on a completed run (result populated; check result->timed_out
 * and result->exit_code). Returns 0 on a setup/spawn failure or when the
 * pseudoconsole capability is absent, with a reason written to errMsg.
 *
 *   cmdLine     full command line (lpApplicationName is NULL, Q14)
 *   cwd         working directory, or NULL to inherit
 *   cols, rows  pseudo console dimensions
 *   timeoutMs   wall-clock kill deadline; 0 = wait indefinitely
 *   stdinBytes  bytes to feed the child's input, or NULL
 *   stdinLen    length of stdinBytes
 *   outputBuf   capture buffer for the merged stream
 *   outputBufSize  capacity of outputBuf
 *   result      populated on a completed run
 *   errMsg      reason on failure (may be empty on success)
 *   errSize     capacity of errMsg
 */
int PtyExecRun(
    const char *cmdLine,
    const char *cwd,
    int cols, int rows,
    int timeoutMs,
    const unsigned char *stdinBytes, int stdinLen,
    unsigned char *outputBuf, int outputBufSize,
    PtyExecResult *result,
    char *errMsg, int errSize
);

#endif /* PTY_EXEC_H */
