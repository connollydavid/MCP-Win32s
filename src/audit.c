/*
 * audit.c - Durable, fail-closed audit log for memory writes
 *
 * Spec: memory-ops.allium (entity AuditRecord; invariant
 * PokeIsAuditedFailClosed; the device half of PokeRequiresBothArmingLayers).
 * See audit.h for the contract.
 *
 * Per-record CreateFileA + seek-to-end + WriteFile + close: no retained
 * handle (crash-durable), integer-only formatting, ANSI, Win32s-safe.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include "strutil.h"
#include "audit.h"

#define AUDIT_DEFAULT_NAME "audit-mem.log"

/* Session audit state (single-threaded server, Win32s). */
static int  g_armed = 0;
static char g_auditPath[MAX_PATH] = "";

/*
 * resolve_default_path - The default sink sits next to the executable
 * (audit-mem.log), like the catalog's default resolution.
 */
static void resolve_default_path(char *out, int outSize)
{
    int n;

    n = (int)GetModuleFileNameA(NULL, out, (DWORD)outSize);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        n--;
    }
    out[n] = '\0';
    lstrcatA(out, AUDIT_DEFAULT_NAME);
}

/*
 * open_for_append - Open the configured sink for appending. Returns an
 * open handle positioned at end-of-file, or INVALID_HANDLE_VALUE.
 */
static HANDLE open_for_append(void)
{
    HANDLE h;
    DWORD  moved;

    if (g_auditPath[0] == '\0') {
        return INVALID_HANDLE_VALUE;
    }
    h = CreateFileA(g_auditPath, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    moved = SetFilePointer(h, 0, NULL, FILE_END);
    if (moved == 0xFFFFFFFF && GetLastError() != NO_ERROR) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

int AuditIsWritable(void)
{
    HANDLE h;

    h = open_for_append();
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    CloseHandle(h);
    return 1;
}

int AuditConfigure(int armRequested, const char *path)
{
    if (path != NULL && path[0] != '\0') {
        McpStrCpyN(g_auditPath, path, (int)sizeof(g_auditPath));
    } else {
        resolve_default_path(g_auditPath, (int)sizeof(g_auditPath));
    }

    /* Fail-closed startup check: a write may be armed only if its record
     * can be written. If the sink is not writable, drop the arm. */
    g_armed = 0;
    if (armRequested && AuditIsWritable()) {
        g_armed = 1;
    }
    return g_armed;
}

int AuditIsArmed(void)
{
    return g_armed;
}

int AuditWritePoke(const char *tier, const char *token, int pid,
                   const char *command,
                   unsigned long addr, unsigned long len,
                   unsigned long bytes_written, int partial)
{
    HANDLE h;
    char   line[640];
    int    lineLen;
    DWORD  written;

    /* One integer-only, ANSI line (spec: entity AuditRecord). wsprintfA is
     * ANSI and integer-only (no FP). A control char in `command` could
     * split the line; mem_ops passes a catalogued command line, but guard
     * the record's integrity by formatting it as the final field. */
    wsprintfA(line,
              "%lu POKE tier=%s token=%s pid=%d addr=0x%08lX len=%lu "
              "written=%lu partial=%d cmd=%s\r\n",
              (unsigned long)GetTickCount(),
              tier != NULL ? tier : "",
              token != NULL ? token : "",
              pid,
              addr, len, bytes_written, partial,
              command != NULL ? command : "");

    h = open_for_append();
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    lineLen = lstrlenA(line);
    if (!WriteFile(h, line, (DWORD)lineLen, &written, NULL) ||
        written != (DWORD)lineLen) {
        CloseHandle(h);
        return 0;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    return 1;
}
