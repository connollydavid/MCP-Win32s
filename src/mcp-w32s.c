/*
 * mcp-w32s.c - MCP Win32s shell: main executable
 *
 * Model Context Protocol server for Win32 systems.
 * Reads newline-delimited JSON commands from a transport (serial/TCP/pipe),
 * dispatches them, and writes JSON responses back.
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
#include "serial.h"
#include "base64.h"
#include "file_ops.h"

/* Protocol constants */
#define READY_MESSAGE   "MCP_WIN32S_READY\n"
#define CMD_BUF_SIZE    8192

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
 * with the line content (null-terminated, newline stripped).
 * Partial lines are accumulated until the next call.
 *
 * Parameters:
 *   buf      - the line buffer state
 *   input    - new bytes to process
 *   inputLen - number of bytes in input
 *   handler  - function called for each complete line
 *   hOutput  - handle passed through to handler (for writing responses)
 *
 * Returns: number of complete lines processed.
 */
int ProcessBuffer(LineBuffer *buf, const char *input, int inputLen,
                  void (*handler)(const char *line, HANDLE hOutput),
                  HANDLE hOutput)
{
    int i;
    int lines;

    lines = 0;
    for (i = 0; i < inputLen; i++) {
        if (input[i] == '\n') {
            buf->data[buf->pos] = '\0';
            if (handler != NULL) {
                handler(buf->data, hOutput);
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
 * Currently a stub that acknowledges commands. Full command dispatch
 * (exec, read, write, list, delete) will be implemented in Phase 3.
 *
 * Parameters:
 *   line    - null-terminated JSON command string
 *   hOutput - handle to write response to (serial port, pipe, etc.)
 */
void ProcessCommand(const char *line, HANDLE hOutput)
{
    JsonCommand cmd;
    char response[MCP_MAX_RESPONSE];
    int responseLen;
    DWORD bytesWritten;
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
        if (responseLen > 0 && hOutput != INVALID_HANDLE_VALUE) {
            WriteFile(hOutput, response, (DWORD)responseLen,
                      &bytesWritten, NULL);
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

    if (responseLen > 0 && hOutput != INVALID_HANDLE_VALUE) {
        WriteFile(hOutput, response, (DWORD)responseLen,
                  &bytesWritten, NULL);
    }
}

#ifndef TEST_BUILD
/*
 * MainLoop - Read from transport and dispatch commands.
 *
 * Reads one byte at a time, feeds into the line buffer,
 * and dispatches complete lines to ProcessCommand.
 *
 * Parameters:
 *   hTransport - handle to read from and write to
 */
static void MainLoop(HANDLE hTransport)
{
    LineBuffer buf;
    char ch;
    DWORD bytesRead;

    memset(&buf, 0, sizeof(buf));

    while (ReadFile(hTransport, &ch, 1, &bytesRead, NULL) && bytesRead > 0) {
        ProcessBuffer(&buf, &ch, 1, ProcessCommand, hTransport);
    }
}

/*
 * SendReady - Write the ready message to the transport.
 */
static void SendReady(HANDLE hTransport)
{
    DWORD bytesWritten;
    int len;

    len = 0;
    while (READY_MESSAGE[len] != '\0') {
        len++;
    }
    WriteFile(hTransport, READY_MESSAGE, (DWORD)len, &bytesWritten, NULL);
}

int main(void)
{
    TransportConfig config;
    HANDLE hTransport;
    const char *cmdLine;

    cmdLine = GetCommandLineA();
    if (!ParseCommandLine(cmdLine, &config)) {
        MessageBoxA(NULL, "Invalid command line arguments.\n\n"
                    "Usage: mcp-w32s.exe [/SERIAL:COMx] [/TCP:port] "
                    "[/PIPE:\\\\.\\pipe\\name]",
                    "MCP-Win32s", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Currently only serial transport is implemented */
    if (config.transport == TRANSPORT_SERIAL) {
        hTransport = OpenSerialPort(config.port, config.baudRate);
        if (hTransport == INVALID_HANDLE_VALUE) {
            MessageBoxA(NULL, "Failed to open serial port.",
                        "MCP-Win32s", MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        MessageBoxA(NULL, "Only serial transport is currently supported.\n"
                    "Use /SERIAL:COMx or run with no arguments for COM1.",
                    "MCP-Win32s", MB_OK | MB_ICONERROR);
        return 1;
    }

    SendReady(hTransport);
    MainLoop(hTransport);
    CloseSerialPort(hTransport);

    return 0;
}
#endif /* TEST_BUILD */
