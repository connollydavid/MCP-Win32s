/*
 * mcp-w32s.c - MCP Win32s shell: main executable
 *
 * Model Context Protocol server for Win32 systems.
 * Reads newline-delimited JSON commands from a transport (serial/TCP/...),
 * dispatches them, and writes JSON responses back. All protocol I/O goes
 * through the backend-agnostic Transport vtable (transport.h) - the core
 * never touches a raw HANDLE or SOCKET.
 *
 * Runs unmodified on Windows 3.1 + Win32s 1.25a through Windows 11.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <string.h>
#include "common.h"
#include "json_parser.h"
#include "transport.h"
#include "serial.h"
#include "tcp.h"
#include "base64.h"
#include "file_ops.h"
#include "feat.h"
#include "ready.h"
#include "argv.h"
#include "binfmt.h"
#include "catalog.h"
#include "exec_ops.h"
#include "pty_exec.h"
#include "toolchain_probe.h"
#include "mem_ops.h"
#include "audit.h"

/* Protocol constants */
#define CMD_BUF_SIZE    8192
#define READ_CHUNK      256

/* Exec admission config (spec: process-ops.allium config block) */
#define EXEC_STDIN_MAX          4096    /* one pipe buffer (Q2) */
#define EXEC_DEFAULT_TIMEOUT_MS 55000   /* inside MCP SDK's 60s window */
#define EXEC_MAX_TIMEOUT_MS     600000  /* hard ceiling; never unbounded */
#define EXEC_OUTPUT_CAP         65536   /* per stream; clamps silently */

/* Shell command-line cap is tighter than CreateProcessA's (Q7) */
#define EXEC_SHELL_CMDLINE_MAX  8192

/* Session exec state: the catalog gate (spec: catalog.allium, fixed
 * per session) and the still_active orphan domain (spec:
 * process-ops.allium - one busy domain shared by exec and ptyExec). */
static Catalog *g_catalog = NULL;
static int g_unsafeMode = 0;
static HANDLE g_orphanHandle = NULL;
static DWORD g_orphanStartTick = 0;
static char g_orphanCmdLine[MCP_MAX_LINE];

/*
 * ExecConfigure - Install the session catalog and /UNSAFE mode. Called
 * once from main after CatalogLoad; tests call it directly.
 */
void ExecConfigure(Catalog *cat, int unsafeMode)
{
    g_catalog = cat;
    g_unsafeMode = unsafeMode;
}

#ifdef TEST_BUILD
/*
 * ExecInjectOrphanForTest - Test hook: install a fake still_active
 * orphan so integration tests can exercise the busy/reap path without
 * a real 16-bit child (obligation: rule-success.ExecRequestBusy,
 * rule-success.OrphanReaped).
 */
void ExecInjectOrphanForTest(HANDLE h, DWORD startTick, const char *cmdLine)
{
    g_orphanHandle = h;
    g_orphanStartTick = startTick;
    lstrcpynA(g_orphanCmdLine, cmdLine != NULL ? cmdLine : "", MCP_MAX_LINE);
}
#endif

/*
 * resp_append / resp_append_int - bounds-checked response assembly for
 * the multi-key exec/ptyExec responses (BuildJsonResponse only does
 * single-key bodies).
 */
static int resp_append(char *dst, int dstSize, int *pos, const char *src)
{
    int len;
    int i;

    len = lstrlenA(src);
    if (*pos + len >= dstSize) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        dst[*pos + i] = src[i];
    }
    *pos += len;
    dst[*pos] = '\0';
    return 1;
}

static int resp_append_int(char *dst, int dstSize, int *pos, long value)
{
    char buf[16];

    wsprintfA(buf, "%ld", value);
    return resp_append(dst, dstSize, pos, buf);
}

/*
 * binary_type_name / killed_by_name - JSON vocabulary (spec:
 * process-ops.allium enums BinaryType / KilledBy).
 */
static const char *binary_type_name(int binType)
{
    switch (binType) {
    case BIN_PE32:  return "pe32";
    case BIN_NE16:  return "ne16";
    case BIN_MZ:    return "mz";
    case BIN_SHELL: return "shell-builtin";
    default:        return "unknown";
    }
}

static const char *killed_by_name(int killedBy)
{
    switch (killedBy) {
    case EXEC_KILLED_TIMEOUT:    return "timeout";
    case EXEC_KILLED_CTRL_BREAK: return "ctrl_break";
    case EXEC_KILLED_MEMORY_CAP: return "memory_cap";
    case EXEC_KILLED_CPU_CAP:    return "cpu_cap";
    default:                     return "";
    }
}

/*
 * send_exec_error - One-key error response for an exec rejection
 * (spec: mcp-protocol.allium rule ExecRejectedResponse).
 */
static void send_exec_error(const char *id, const char *reason, Transport *t)
{
    static char response[MCP_MAX_RESPONSE];
    int len;

    len = BuildJsonResponse(id, "error", "error", reason,
                            response, sizeof(response));
    if (len > 0 && t != NULL) {
        TransportWriteAll(t, response, len);
    }
}

/*
 * send_busy_error - The informative busy rejection: carries the
 * blocking still_active child's command line and elapsed ms (spec:
 * process-ops.allium rule ExecRequestBusy, decision 9).
 */
static void send_busy_error(const char *id, Transport *t)
{
    static char response[MCP_MAX_RESPONSE];
    static char escaped[MCP_MAX_LINE * 2];
    int pos;

    if (JsonEscape(g_orphanCmdLine, escaped, (int)sizeof(escaped)) < 0) {
        escaped[0] = '\0';
    }

    pos = 0;
    if (!resp_append(response, sizeof(response), &pos, "{\"id\":\"") ||
        !resp_append(response, sizeof(response), &pos, id) ||
        !resp_append(response, sizeof(response), &pos,
                     "\",\"status\":\"error\",\"error\":\"busy\","
                     "\"blocking_cmd_line\":\"") ||
        !resp_append(response, sizeof(response), &pos, escaped) ||
        !resp_append(response, sizeof(response), &pos, "\",\"elapsed_ms\":") ||
        !resp_append_int(response, sizeof(response), &pos,
                         (long)(GetTickCount() - g_orphanStartTick)) ||
        !resp_append(response, sizeof(response), &pos, "}\n")) {
        return;
    }
    if (t != NULL) {
        TransportWriteAll(t, response, pos);
    }
}

/*
 * HandleExec - The exec / ptyExec dispatcher: busy domain, catalog
 * gate, admission sentinels, spawn, response (specs: process-ops,
 * catalog, mcp-protocol).
 */
static void HandleExec(JsonCommand *cmd, Transport *t, int isPty)
{
    /* Static: large buffers, single-threaded server. */
    static char cmdLine[32768];
    static char joined[32768];
    static unsigned char stdinBuf[EXEC_STDIN_MAX + 2048];
    static unsigned char stdoutBuf[EXEC_OUTPUT_CAP];
    static unsigned char stderrBuf[EXEC_OUTPUT_CAP];
    static char b64Out[(EXEC_OUTPUT_CAP / 3 + 1) * 4 + 8];
    static char b64Err[(EXEC_OUTPUT_CAP / 3 + 1) * 4 + 8];
    static char response[MCP_MAX_RESPONSE];
    const char *argvPtrs[MCP_MAX_ARGV];
    char name[MCP_MAX_ARG_LEN];
    char errMsg[160];
    const CatalogEntry *entry;
    ExecResult res;
    PtyExecResult pres;
    BinaryType binType;
    int unsafeUsed;
    int viaShell;
    int stdinLen;
    int timeoutMs;
    int outputCap;
    int cmdLineMax;
    int i;
    int pos;

    /* 1. Busy domain: re-poll the retained still_active handle on
     * every request; exited -> implicit reap, request proceeds;
     * still running -> informative busy (spec: ExecRequestBusy /
     * OrphanReaped; one busy domain for exec AND ptyExec). */
    if (g_orphanHandle != NULL) {
        DWORD code;
        if (GetExitCodeProcess(g_orphanHandle, &code) &&
            code == STILL_ACTIVE) {
            send_busy_error(cmd->id, t);
            return;
        }
        CloseHandle(g_orphanHandle);
        g_orphanHandle = NULL;
        g_orphanCmdLine[0] = '\0';
    }

    /* 2. Command name: argv[0] preferred, first token of line legacy. */
    name[0] = '\0';
    if (cmd->argv_count > 0) {
        lstrcpynA(name, cmd->argv[0], (int)sizeof(name));
    } else {
        int n;
        n = 0;
        while (cmd->line[n] != '\0' && cmd->line[n] != ' ' &&
               n < (int)sizeof(name) - 1) {
            name[n] = cmd->line[n];
            n++;
        }
        name[n] = '\0';
    }
    if (name[0] == '\0') {
        send_exec_error(cmd->id, "spawn failed: empty command line", t);
        return;
    }

    /* 3. stdin: decode, then enforce the one-pipe-buffer cap (spec:
     * config stdin_max; rule ExecStdinTooLarge). */
    stdinLen = 0;
    if (cmd->stdin_b64[0] != '\0') {
        stdinLen = Base64Decode(cmd->stdin_b64, stdinBuf,
                                (int)sizeof(stdinBuf));
        if (stdinLen < 0) {
            send_exec_error(cmd->id, "invalid base64", t);
            return;
        }
        if (stdinLen > EXEC_STDIN_MAX) {
            send_exec_error(cmd->id, "stdin too large", t);
            return;
        }
    }

    /* 4. ptyExec capability gate before the catalog (spec: rule
     * PtyUnavailable; admission order busy > pty > catalog). */
    if (isPty && !g_features.has_create_pseudo_console) {
        send_exec_error(cmd->id, "pty not available on this Windows", t);
        return;
    }

    /* 5. Catalog gate (spec: catalog.allium; per-request unsafe
     * bypasses, /UNSAFE or missing catalog disables enforcement). */
    unsafeUsed = 0;
    viaShell = (cmd->shell_flag != 0);
    entry = NULL;
    if (cmd->unsafe_flag) {
        unsafeUsed = 1;            /* rule GateBypassedByUnsafeRequest */
    } else if (g_unsafeMode || g_catalog == NULL) {
        ;                          /* rule GateUnenforced */
    } else {
        entry = CatalogLookup(g_catalog, name);
        if (entry == NULL) {
            send_exec_error(cmd->id, "command not in catalog", t);
            return;                /* rule GateMiss */
        }
        if (cmd->argv_count > 0) {
            for (i = 0; i < cmd->argv_count; i++) {
                argvPtrs[i] = cmd->argv[i];
            }
            if (!CatalogValidateArgs(entry, argvPtrs, cmd->argv_count,
                                     errMsg, (int)sizeof(errMsg))) {
                send_exec_error(cmd->id, "argument not allowed", t);
                return;            /* rule GateArgsInvalid */
            }
        }
        if (CatalogEntryIsBuiltin(entry)) {
            viaShell = 1;          /* rule GateBuiltinHit: auto-route */
        }
    }

    /* 6. Build the command line: argv preferred (pre-decision 1).
     *
     * A shell route (builtin auto-route OR an external shell request)
     * hands its command tail to cmd.exe/command.com, so every
     * cmd metacharacter (& | < > ^ ( ) %) in a USER-supplied token must
     * be caret-escaped (Q15) or it becomes a command separator - a
     * catalog-gate bypass (e.g. argv ["dir","x&calc"] running calc).
     * The shell prefix itself ("command.com /c dir") comes from the
     * catalog and is trusted; only the user argument tail is escaped.
     * Direct (non-shell) spawns pass argv straight to CreateProcessA
     * and need only ArgvJoin quoting. */
    {
        /* The catalog's builtin shell string ("<shell> /c <name>") is
         * used as the trusted prefix only for the argv form, where the
         * builtin name is argv[0] and the escaped user tail is
         * argv[1:]. A legacy line-only request carries the name inside
         * cmd.line, so it uses the generic era shell prefix over the
         * whole escaped line - never the catalog string (which would
         * double the command name). */
        int useBuiltinPrefix;
        int skip;
        int jl;

        useBuiltinPrefix = (entry != NULL && CatalogEntryIsBuiltin(entry)
                            && cmd->argv_count > 0);
        skip = useBuiltinPrefix ? 1 : 0;
        joined[0] = '\0';

        if (cmd->argv_count > 0) {
            for (i = skip; i < cmd->argv_count; i++) {
                argvPtrs[i - skip] = cmd->argv[i];
            }
            jl = ArgvJoin(argvPtrs, cmd->argv_count - skip,
                          joined, (int)sizeof(joined));
            if (jl < 0) {
                send_exec_error(cmd->id, "invalid argv", t);
                return;
            }
        } else {
            lstrcpynA(joined, cmd->line, (int)sizeof(joined));
        }

        if (viaShell) {
            /* Caret-escape the user tail (Q15), then prepend the
             * trusted shell prefix. */
            static char escapedTail[32768];
            const char *prefix;

            if (ArgvCmdEscape(joined, escapedTail,
                              (int)sizeof(escapedTail)) < 0) {
                send_exec_error(cmd->id, "command line too long", t);
                return;
            }
            if (useBuiltinPrefix) {
                prefix = g_features.is_win32s
                    ? CatalogEntryShellWin32s(entry)
                    : CatalogEntryShellModern(entry);
            } else {
                prefix = g_features.is_win32s
                    ? "command.com /c" : "cmd.exe /c";
            }
            lstrcpynA(cmdLine, prefix, (int)sizeof(cmdLine));
            if (escapedTail[0] != '\0') {
                if (lstrlenA(cmdLine) + 1 + lstrlenA(escapedTail) >=
                    (int)sizeof(cmdLine)) {
                    send_exec_error(cmd->id, "command line too long", t);
                    return;
                }
                lstrcatA(cmdLine, " ");
                lstrcatA(cmdLine, escapedTail);
            }
        } else {
            lstrcpynA(cmdLine, joined, (int)sizeof(cmdLine));
        }
    }

    /* 7. Length caps (Q7): 8192 via shell, 32766 direct. */
    cmdLineMax = viaShell ? EXEC_SHELL_CMDLINE_MAX : 32766;
    if (lstrlenA(cmdLine) > cmdLineMax) {
        send_exec_error(cmd->id, "command line too long", t);
        return;
    }

    /* 8. Sentinel resolution (spec: rule ExecRequestAccepted -
     * effective_timeout_ms / effective_output_cap). */
    timeoutMs = cmd->timeout_ms;
    if (timeoutMs <= 0) {
        timeoutMs = EXEC_DEFAULT_TIMEOUT_MS;
    } else if (timeoutMs > EXEC_MAX_TIMEOUT_MS) {
        timeoutMs = EXEC_MAX_TIMEOUT_MS;
    }
    outputCap = cmd->max_output;
    if (outputCap <= 0 || outputCap > EXEC_OUTPUT_CAP) {
        outputCap = EXEC_OUTPUT_CAP;
    }

    /* 9. Binary classification (Q16): builtins skip the file read. */
    if (entry != NULL && CatalogEntryIsBuiltin(entry)) {
        binType = BIN_SHELL;
    } else if (viaShell) {
        binType = BIN_SHELL;
    } else if (!BinFmtClassify(name, &binType, errMsg,
                               (int)sizeof(errMsg))) {
        binType = BIN_UNKNOWN;
    }

    /* 10. Run. */
    if (isPty) {
        int cols;
        int rows;
        cols = cmd->cols > 0 ? cmd->cols : 80;
        rows = cmd->rows > 0 ? cmd->rows : 25;
        if (!PtyExecRun(cmdLine, cmd->cwd[0] ? cmd->cwd : NULL,
                        cols, rows, timeoutMs,
                        stdinLen > 0 ? stdinBuf : NULL, stdinLen,
                        stdoutBuf, outputCap, &pres,
                        errMsg, (int)sizeof(errMsg))) {
            send_exec_error(cmd->id, errMsg, t);
            return;
        }
        if (pres.timed_out) {
            send_exec_error(cmd->id, "timed out", t);
            return;
        }
        if (Base64Encode(stdoutBuf, pres.output_len, b64Out,
                         (int)sizeof(b64Out)) < 0) {
            send_exec_error(cmd->id, "encode error", t);
            return;
        }
        pos = 0;
        if (!resp_append(response, sizeof(response), &pos, "{\"id\":\"") ||
            !resp_append(response, sizeof(response), &pos, cmd->id) ||
            !resp_append(response, sizeof(response), &pos,
                         "\",\"status\":\"ok\",\"exit_code\":") ||
            !resp_append_int(response, sizeof(response), &pos,
                             pres.exit_code) ||
            !resp_append(response, sizeof(response), &pos,
                         ",\"output_b64\":\"") ||
            !resp_append(response, sizeof(response), &pos, b64Out) ||
            !resp_append(response, sizeof(response), &pos,
                         "\",\"output_kind\":\"ansi\",\"output_truncated\":") ||
            !resp_append(response, sizeof(response), &pos,
                         pres.output_truncated ? "true" : "false") ||
            !resp_append(response, sizeof(response), &pos,
                         ",\"duration_ms\":") ||
            !resp_append_int(response, sizeof(response), &pos,
                             pres.duration_ms) ||
            !resp_append(response, sizeof(response), &pos,
                         unsafeUsed ? ",\"unsafe_used\":true}\n" : "}\n")) {
            return;
        }
        if (t != NULL) {
            TransportWriteAll(t, response, pos);
        }
        return;
    }

    if (!ExecOpRun(cmdLine, cmd->cwd[0] ? cmd->cwd : NULL,
                   timeoutMs, 1,
                   stdinLen > 0 ? stdinBuf : NULL, stdinLen,
                   stdoutBuf, outputCap, stderrBuf, outputCap,
                   cmd->mem_cap_bytes, cmd->cpu_time_ms,
                   (int)binType, &res, errMsg, (int)sizeof(errMsg))) {
        send_exec_error(cmd->id, errMsg, t);
        return;
    }

    /* still_active: the deliberately unkilled 16-bit child becomes the
     * session orphan; the request still gets its in-band timed-out
     * error (spec: rule VdmTimeoutNotKilled). */
    if (res.still_active) {
        g_orphanHandle = res.orphan_handle;
        g_orphanStartTick = res.orphan_start_tick;
        lstrcpynA(g_orphanCmdLine, cmdLine, MCP_MAX_LINE);
        send_exec_error(cmd->id, "timed out", t);
        return;
    }

    if (res.timed_out) {
        send_exec_error(cmd->id, "timed out", t);
        return;
    }

    if (Base64Encode(stdoutBuf, res.stdout_len, b64Out,
                     (int)sizeof(b64Out)) < 0 ||
        Base64Encode(stderrBuf, res.stderr_len, b64Err,
                     (int)sizeof(b64Err)) < 0) {
        send_exec_error(cmd->id, "encode error", t);
        return;
    }

    pos = 0;
    if (!resp_append(response, sizeof(response), &pos, "{\"id\":\"") ||
        !resp_append(response, sizeof(response), &pos, cmd->id) ||
        !resp_append(response, sizeof(response), &pos,
                     "\",\"status\":\"ok\",\"exit_code\":") ||
        !resp_append_int(response, sizeof(response), &pos, res.exit_code) ||
        !resp_append(response, sizeof(response), &pos,
                     ",\"stdout_b64\":\"") ||
        !resp_append(response, sizeof(response), &pos, b64Out) ||
        !resp_append(response, sizeof(response), &pos,
                     "\",\"stderr_b64\":\"") ||
        !resp_append(response, sizeof(response), &pos, b64Err) ||
        !resp_append(response, sizeof(response), &pos,
                     "\",\"stdout_truncated\":") ||
        !resp_append(response, sizeof(response), &pos,
                     res.stdout_truncated ? "true" : "false") ||
        !resp_append(response, sizeof(response), &pos,
                     ",\"stderr_truncated\":") ||
        !resp_append(response, sizeof(response), &pos,
                     res.stderr_truncated ? "true" : "false") ||
        !resp_append(response, sizeof(response), &pos,
                     ",\"duration_ms\":") ||
        !resp_append_int(response, sizeof(response), &pos,
                         res.duration_ms) ||
        !resp_append(response, sizeof(response), &pos,
                     ",\"exec_method\":\"") ||
        !resp_append(response, sizeof(response), &pos,
                     (binType == BIN_NE16 || binType == BIN_MZ)
                         ? "vdm-best-effort"
                         : (viaShell ? "shell" : "direct")) ||
        !resp_append(response, sizeof(response), &pos,
                     "\",\"binary_type\":\"") ||
        !resp_append(response, sizeof(response), &pos,
                     binary_type_name((int)binType)) ||
        !resp_append(response, sizeof(response), &pos,
                     "\",\"killed_by\":\"") ||
        !resp_append(response, sizeof(response), &pos,
                     killed_by_name(res.killed_by)) ||
        !resp_append(response, sizeof(response), &pos,
                     unsafeUsed ? "\",\"unsafe_used\":true}\n" : "\"}\n")) {
        return;
    }
    if (t != NULL) {
        TransportWriteAll(t, response, pos);
    }
}

/*
 * HandleMem - The memory peek/poke dispatcher (spec: memory-ops.allium).
 * Routes the five wire verbs (spawnRetain/peek/poke/terminate/release) to
 * the mem_ops module and builds their rich JSON responses. The module owns
 * the catalog gate (spawnRetain), the address/range/region floor, the
 * /ALLOWMEMWRITE arm and the fail-closed audit; the dispatcher only parses
 * the wire fields and shapes the reply. Errors are recoverable (the bridge
 * maps a status:error to an isError tool result).
 */
static void HandleMem(JsonCommand *cmd, Transport *t)
{
    /* Static: single-threaded server (Win32s); these are large. */
    static unsigned char memBuf[MEM_MAX_ACCESS];
    static char b64[(MEM_MAX_ACCESS / 3 + 1) * 4 + 8];
    static char response[MCP_MAX_RESPONSE];
    const char *argvPtrs[MCP_MAX_ARGV];
    const char *token;
    unsigned long addr;
    unsigned long len;
    int pos;
    int i;

    token = (cmd->mem_token[0] != '\0') ? cmd->mem_token : NULL;

    if (strcmp(cmd->cmd, "spawnRetain") == 0) {
        MemSpawnResult sr;
        for (i = 0; i < cmd->argv_count && i < MCP_MAX_ARGV; i++) {
            argvPtrs[i] = cmd->argv[i];
        }
        MemSpawnRetain(g_catalog, g_unsafeMode, argvPtrs, cmd->argv_count, &sr);
        if (!sr.ok) {
            send_exec_error(cmd->id, sr.reason, t);
            return;
        }
        pos = 0;
        if (!resp_append(response, sizeof(response), &pos, "{\"id\":\"") ||
            !resp_append(response, sizeof(response), &pos, cmd->id) ||
            !resp_append(response, sizeof(response), &pos,
                         "\",\"status\":\"ok\",\"token\":\"") ||
            !resp_append(response, sizeof(response), &pos, sr.token) ||
            !resp_append(response, sizeof(response), &pos, "\",\"pid\":") ||
            !resp_append_int(response, sizeof(response), &pos, (long)sr.pid) ||
            !resp_append(response, sizeof(response), &pos, "}\n")) {
            return;
        }
        if (t != NULL) {
            TransportWriteAll(t, response, pos);
        }
        return;
    }

    if (strcmp(cmd->cmd, "peek") == 0) {
        MemPeekResult pr;
        int b64Len;
        if (!MemParseU32(cmd->mem_addr, &addr)) {
            send_exec_error(cmd->id, "invalid address", t);
            return;
        }
        if (!MemParseU32(cmd->mem_len, &len)) {
            send_exec_error(cmd->id, "invalid length", t);
            return;
        }
        MemPeek(token, addr, len, memBuf, &pr);
        if (!pr.ok) {
            send_exec_error(cmd->id, pr.reason, t);
            return;
        }
        b64Len = Base64Encode(memBuf, (int)pr.bytes_read, b64, (int)sizeof(b64));
        if (b64Len < 0) {
            send_exec_error(cmd->id, "encode error", t);
            return;
        }
        pos = 0;
        if (!resp_append(response, sizeof(response), &pos, "{\"id\":\"") ||
            !resp_append(response, sizeof(response), &pos, cmd->id) ||
            !resp_append(response, sizeof(response), &pos,
                         "\",\"status\":\"ok\",\"data_b64\":\"") ||
            !resp_append(response, sizeof(response), &pos, b64) ||
            !resp_append(response, sizeof(response), &pos,
                         "\",\"bytes_read\":") ||
            !resp_append_int(response, sizeof(response), &pos,
                             (long)pr.bytes_read) ||
            !resp_append(response, sizeof(response), &pos,
                         ",\"truncated\":") ||
            !resp_append(response, sizeof(response), &pos,
                         pr.truncated ? "true" : "false") ||
            !resp_append(response, sizeof(response), &pos, "}\n")) {
            return;
        }
        if (t != NULL) {
            TransportWriteAll(t, response, pos);
        }
        return;
    }

    if (strcmp(cmd->cmd, "poke") == 0) {
        MemPokeResult pr;
        int decoded;
        if (!MemParseU32(cmd->mem_addr, &addr)) {
            send_exec_error(cmd->id, "invalid address", t);
            return;
        }
        decoded = Base64Decode(cmd->data, memBuf, (int)sizeof(memBuf));
        if (decoded < 0) {
            send_exec_error(cmd->id, "invalid base64", t);
            return;
        }
        MemPoke(token, addr, memBuf, (unsigned long)decoded, &pr);
        if (!pr.ok) {
            send_exec_error(cmd->id, pr.reason, t);
            return;
        }
        pos = 0;
        if (!resp_append(response, sizeof(response), &pos, "{\"id\":\"") ||
            !resp_append(response, sizeof(response), &pos, cmd->id) ||
            !resp_append(response, sizeof(response), &pos,
                         "\",\"status\":\"ok\",\"bytes_written\":") ||
            !resp_append_int(response, sizeof(response), &pos,
                             (long)pr.bytes_written) ||
            !resp_append(response, sizeof(response), &pos, ",\"partial\":") ||
            !resp_append(response, sizeof(response), &pos,
                         pr.partial ? "true" : "false") ||
            !resp_append(response, sizeof(response), &pos, "}\n")) {
            return;
        }
        if (t != NULL) {
            TransportWriteAll(t, response, pos);
        }
        return;
    }

    if (strcmp(cmd->cmd, "terminate") == 0) {
        if (!MemTerminate(cmd->mem_token)) {
            send_exec_error(cmd->id, "invalid or consumed token", t);
            return;
        }
        pos = 0;
        if (resp_append(response, sizeof(response), &pos, "{\"id\":\"") &&
            resp_append(response, sizeof(response), &pos, cmd->id) &&
            resp_append(response, sizeof(response), &pos,
                        "\",\"status\":\"ok\",\"terminated\":true}\n") &&
            t != NULL) {
            TransportWriteAll(t, response, pos);
        }
        return;
    }

    if (strcmp(cmd->cmd, "release") == 0) {
        if (!MemRelease(cmd->mem_token)) {
            send_exec_error(cmd->id, "invalid or consumed token", t);
            return;
        }
        pos = 0;
        if (resp_append(response, sizeof(response), &pos, "{\"id\":\"") &&
            resp_append(response, sizeof(response), &pos, cmd->id) &&
            resp_append(response, sizeof(response), &pos,
                        "\",\"status\":\"ok\",\"released\":true}\n") &&
            t != NULL) {
            TransportWriteAll(t, response, pos);
        }
        return;
    }
}

/*
 * LineBuffer - Accumulates characters and splits on newline boundaries.
 */
typedef struct {
    char data[CMD_BUF_SIZE];
    int pos;
} LineBuffer;

/*
 * ProcessBuffer - Feed a chunk of bytes into the line buffer.
 *
 * For each complete line (terminated by '\n'), calls the handler function
 * with the line content (null-terminated, newline stripped) and the
 * transport to write responses to. Partial lines are accumulated.
 *
 * Returns: number of complete lines processed.
 */
int ProcessBuffer(LineBuffer *buf, const char *input, int inputLen,
                  void (*handler)(const char *line, Transport *t),
                  Transport *t)
{
    int i;
    int lines;

    lines = 0;
    for (i = 0; i < inputLen; i++) {
        if (input[i] == '\n') {
            buf->data[buf->pos] = '\0';
            if (handler != NULL) {
                handler(buf->data, t);
            }
            buf->pos = 0;
            lines++;
        } else if (buf->pos < CMD_BUF_SIZE - 1) {
            buf->data[buf->pos++] = input[i];
        }
        /* else: overflow, silently drop character */
    }
    return lines;
}

/*
 * ProcessCommand - Parse a JSON command line and write a JSON response.
 *
 * Writes responses through the Transport vtable. A NULL transport skips
 * the write (used by tests that only exercise parse/dispatch).
 */
void ProcessCommand(const char *line, Transport *t)
{
    /* Static: JsonCommand now carries the 32KB argv block and the
     * response buffer is 256KB - far too large for the stack on the
     * i386 target. Single-threaded by hard constraint (Win32s). */
    static JsonCommand cmd;
    static char response[MCP_MAX_RESPONSE];
    int responseLen;
    static unsigned char raw[MCP_MAX_DATA];
    static char b64[MCP_MAX_DATA];
    static char fileList[MCP_MAX_DATA];
    char errMsg[128];
    int rawLen;
    int b64Len;

    if (line == NULL || line[0] == '\0') {
        return;
    }

    if (!ParseJsonCommand(line, &cmd)) {
        responseLen = BuildJsonResponse("", "error", "error",
                                        "invalid JSON", response,
                                        sizeof(response));
        if (responseLen > 0 && t != NULL) {
            TransportWriteAll(t, response, responseLen);
        }
        return;
    }

    if (strcmp(cmd.cmd, "echo") == 0) {
        responseLen = BuildJsonResponse(cmd.id, "ok", "data",
                                        cmd.line, response,
                                        sizeof(response));
    } else if (strcmp(cmd.cmd, "read") == 0) {
        if (FileOpRead(cmd.path, raw, (int)sizeof(raw),
                       &rawLen, errMsg, sizeof(errMsg))) {
            b64Len = Base64Encode(raw, rawLen, b64, (int)sizeof(b64));
            if (b64Len > 0) {
                responseLen = BuildJsonResponse(cmd.id, "ok", "data",
                                                b64, response,
                                                sizeof(response));
            } else {
                responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                                "encode error",
                                                response, sizeof(response));
            }
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "write") == 0) {
        rawLen = Base64Decode(cmd.data, raw, (int)sizeof(raw));
        if (rawLen < 0) {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            "invalid base64",
                                            response, sizeof(response));
        } else if (FileOpWrite(cmd.path, raw, rawLen,
                               errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                            "written",
                                            response, sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "list") == 0) {
        if (FileOpList(cmd.path, fileList, (int)sizeof(fileList),
                       errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "files",
                                            fileList, response,
                                            sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "delete") == 0) {
        if (FileOpDelete(cmd.path, errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                            "deleted",
                                            response, sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "copy") == 0) {
        if (FileOpCopy(cmd.path, cmd.dest, errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                            "copied",
                                            response, sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "move") == 0) {
        if (FileOpMove(cmd.path, cmd.dest, errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                            "moved",
                                            response, sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "mkdir") == 0) {
        if (FileOpMakeDir(cmd.path, errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                            "created",
                                            response, sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "rmdir") == 0) {
        if (FileOpRemoveDir(cmd.path, errMsg, sizeof(errMsg))) {
            responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                            "removed",
                                            response, sizeof(response));
        } else {
            responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                            errMsg, response,
                                            sizeof(response));
        }
    } else if (strcmp(cmd.cmd, "exec") == 0) {
        HandleExec(&cmd, t, 0);
        return;
    } else if (strcmp(cmd.cmd, "ptyExec") == 0) {
        HandleExec(&cmd, t, 1);
        return;
    } else if (strcmp(cmd.cmd, "spawnRetain") == 0 ||
               strcmp(cmd.cmd, "peek") == 0 ||
               strcmp(cmd.cmd, "poke") == 0 ||
               strcmp(cmd.cmd, "terminate") == 0 ||
               strcmp(cmd.cmd, "release") == 0) {
        HandleMem(&cmd, t);
        return;
    } else {
        responseLen = BuildJsonResponse(cmd.id, "error", "error",
                                        "unknown command",
                                        response, sizeof(response));
    }

    if (responseLen > 0 && t != NULL) {
        TransportWriteAll(t, response, responseLen);
    }
}

#ifndef TEST_BUILD
/* Warning surfaced in the ready message when the catalog fails to load
 * (spec: catalog.allium ServerStartup; PHASE4 startup checklist). */
static const char *g_readyWarning = NULL;

/* Build toolchains detected once at startup (spec: toolchains.allium
 * ToolchainDetected); reported in every ready message's features.toolchains. */
static ToolchainSet g_toolchains;

/*
 * SendReady - Write the extended per-connection ready message: status,
 * codepage, version, transport, features (spec: wire-contract.allium
 * contract ReadyHandshake).
 */
static void SendReady(Transport *t)
{
    static char msg[2048];
    int len;

    len = BuildReadyMessage(TransportName(t), g_readyWarning, &g_toolchains,
                            msg, (int)sizeof(msg));
    if (len > 0) {
        TransportWriteAll(t, msg, len);
    }
}

/*
 * Serve - Read from a connection and dispatch commands until it closes.
 */
static void Serve(Transport *t)
{
    LineBuffer buf;
    char chunk[READ_CHUNK];
    int n;

    memset(&buf, 0, sizeof(buf));

    for (;;) {
        n = t->read(t, chunk, (int)sizeof(chunk));
        if (n <= 0) {
            break;
        }
        ProcessBuffer(&buf, chunk, n, ProcessCommand, t);
    }
}

/*
 * LoadCatalogAtStartup - Resolve the catalog path (default: next to the
 * exe; /CATALOG: overrides), load it, and install the exec gate.
 * Failure is non-fatal: the ready message carries the warning.
 */
static Catalog *LoadCatalogAtStartup(const TransportConfig *config)
{
    static char path[MCP_MAX_PATH_LEN];
    Catalog *cat;
    char err[160];

    if (config->catalogPath[0] != '\0') {
        lstrcpynA(path, config->catalogPath, (int)sizeof(path));
    } else {
        int n;
        n = (int)GetModuleFileNameA(NULL, path, sizeof(path));
        while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') {
            n--;
        }
        path[n] = '\0';
        lstrcatA(path, "catalog\\win32-commands.json");
    }

    cat = NULL;
    if (!CatalogLoad(path, &cat, err, (int)sizeof(err))) {
        g_readyWarning = "catalog not loaded";
        return NULL;
    }
    return cat;
}

int main(void)
{
    TransportConfig config;
    Transport listener;
    Transport *conn;
    char err[160];
    const char *cmdLine;
    Catalog *cat;

    /* FeatInit first - before any spawn / catalog load / ready
     * message (PHASE4 startup checklist step 1). */
    FeatInit();

    cmdLine = GetCommandLineA();
    if (!ParseCommandLine(cmdLine, &config)) {
        MessageBoxA(NULL, "Invalid command line arguments.\n\n"
                    "Usage: mcp-w32s.exe [/SERIAL:COMx | /TCP:port | /AUTO[:port]]\n"
                    "                    [/BIND:addr] [/UNSAFE] [/CATALOG:path]\n\n"
                    "  /AUTO    try TCP, fall back to serial\n"
                    "  /BIND    restrict TCP to addr (default: all interfaces)\n"
                    "  /UNSAFE  disable the exec catalog whitelist\n"
                    "  /CATALOG override the catalog file location",
                    "MCP-Win32s", MB_OK | MB_ICONERROR);
        return 1;
    }

    cat = LoadCatalogAtStartup(&config);
    ExecConfigure(cat, config.unsafeMode);

    /* Arm the memory-write path + audit sink (spec: memory-ops.allium - the
     * device /ALLOWMEMWRITE arm + fail-closed audit; off by default). If the
     * arm is requested but the audit log is not writable, AuditConfigure drops
     * the arm (a poke that cannot be logged is never armable). */
    AuditConfigure(config.allowMemWrite, config.auditPath);

    /* Detect installed build toolchains once (spec: toolchains.allium
     * ToolchainDetected): run each catalogued probe command's version banner.
     * Reported in every ready message's features.toolchains array. */
    ToolchainProbe(cat, &g_toolchains);

    SerialBackendRegister();
    TcpBackendRegister();

    if (!TransportOpen(&config, &listener, err, sizeof(err))) {
        MessageBoxA(NULL, err, "MCP-Win32s", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Accept loop. Serial is point-to-point (no accept): one peer, done.
     * TCP is a listener: serve one client, then accept the next. */
    for (;;) {
        conn = listener.accept ? listener.accept(&listener) : &listener;
        if (conn == NULL) {
            break;
        }
        SendReady(conn);
        Serve(conn);
        /* Release any spawn-retained memory targets the client held, so a
         * disconnect never leaks a process handle (spec: memory-ops.allium -
         * MemReleaseAll on connection close). */
        MemReleaseAll();
        if (conn != &listener && conn->close != NULL) {
            conn->close(conn);
        }
        if (listener.accept == NULL) {
            break;
        }
    }

    if (listener.close != NULL) {
        listener.close(&listener);
    }
    TcpBackendCleanup();

    return 0;
}
#endif /* TEST_BUILD */
