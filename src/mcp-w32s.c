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

/* Protocol constants */
#define CMD_BUF_SIZE    8192
#define READ_CHUNK      256

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
    JsonCommand cmd;
    char response[MCP_MAX_RESPONSE];
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
    } else if (strcmp(cmd.cmd, "exec") == 0) {
        responseLen = BuildJsonResponse(cmd.id, "ok", "message",
                                        "command received",
                                        response, sizeof(response));
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
/*
 * SendReady - Write the per-connection ready message naming the backend.
 */
static void SendReady(Transport *t)
{
    char msg[96];
    msg[0] = '\0';
    lstrcatA(msg, "{\"status\":\"ready\",\"transport\":\"");
    lstrcatA(msg, TransportName(t));
    lstrcatA(msg, "\"}\n");
    TransportWriteAll(t, msg, lstrlenA(msg));
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

int main(void)
{
    TransportConfig config;
    Transport listener;
    Transport *conn;
    char err[160];
    const char *cmdLine;

    cmdLine = GetCommandLineA();
    if (!ParseCommandLine(cmdLine, &config)) {
        MessageBoxA(NULL, "Invalid command line arguments.\n\n"
                    "Usage: mcp-w32s.exe [/SERIAL:COMx] [/TCP:port]",
                    "MCP-Win32s", MB_OK | MB_ICONERROR);
        return 1;
    }

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
