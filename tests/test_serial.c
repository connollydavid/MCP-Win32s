/*
 * test_serial.c - Unit tests for serial.c and mcp-w32s.c logic
 *
 * Tests command-line parsing, DCB/timeout configuration,
 * protocol framing (ProcessBuffer), and command dispatch.
 * All tests run without hardware (no actual COM ports needed).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "transport.h"
#include "serial.h"
#include "json_parser.h"
#include "common.h"
#include "mock_transport.h"
#include "feat.h"
#include "catalog.h"
#include "base64.h"

/* ========================================================
 * Forward declarations for functions in mcp-w32s.c
 * that we test directly (linked at compile time).
 * ======================================================== */

#define CMD_BUF_SIZE 8192

typedef struct {
    char data[CMD_BUF_SIZE];
    int pos;
} LineBuffer;

extern int ProcessBuffer(LineBuffer *buf, const char *input, int inputLen,
                         void (*handler)(const char *line, Transport *t),
                         Transport *t);

extern void ProcessCommand(const char *line, Transport *t);

/* Phase 4 exec dispatcher hooks (mcp-w32s.c, TEST_BUILD) */
extern void ExecConfigure(Catalog *cat, int unsafeMode);
extern void ExecInjectOrphanForTest(HANDLE h, DWORD startTick,
                                    const char *cmdLine);

/* ========================================================
 * Test helpers
 * ======================================================== */

/* Capture lines produced by ProcessBuffer */
#define MAX_CAPTURED_LINES 16
#define MAX_LINE_LEN 1024

static char g_captured_lines[MAX_CAPTURED_LINES][MAX_LINE_LEN];
static int g_captured_count = 0;

static void capture_handler(const char *line, Transport *t)
{
    (void)t;
    if (g_captured_count < MAX_CAPTURED_LINES) {
        int i;
        for (i = 0; line[i] != '\0' && i < MAX_LINE_LEN - 1; i++) {
            g_captured_lines[g_captured_count][i] = line[i];
        }
        g_captured_lines[g_captured_count][i] = '\0';
        g_captured_count++;
    }
}

static void reset_capture(void)
{
    int i;
    g_captured_count = 0;
    for (i = 0; i < MAX_CAPTURED_LINES; i++) {
        g_captured_lines[i][0] = '\0';
    }
}

/* ========================================================
 * ParseCommandLine tests
 * ======================================================== */

TEST_CASE(cmdline_serial_com1) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/SERIAL:COM1", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse successfully");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "transport type");
    TEST_ASSERT_STR_EQUAL("COM1", config.port, "port name");
    TEST_ASSERT_INT_EQUAL((int)CBR_115200, (int)config.baudRate, "baud rate");
}

TEST_CASE(cmdline_serial_com2) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/SERIAL:COM2", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse successfully");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "transport type");
    TEST_ASSERT_STR_EQUAL("COM2", config.port, "port name");
}

TEST_CASE(cmdline_serial_lowercase) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/serial:COM3", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "case insensitive flag");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "transport type");
    TEST_ASSERT_STR_EQUAL("COM3", config.port, "port name");
}

TEST_CASE(cmdline_tcp) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/TCP:8932", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse successfully");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_TCP, config.transport, "transport type");
    TEST_ASSERT_INT_EQUAL(8932, config.tcpPort, "tcp port");
}

TEST_CASE(cmdline_tcp_invalid_port) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/TCP:0", &config);
    TEST_ASSERT_INT_EQUAL(0, result, "port 0 should fail");
}

TEST_CASE(cmdline_pipe) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/PIPE:\\\\.\\pipe\\mcp", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse successfully");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_PIPE, config.transport, "transport type");
    TEST_ASSERT_STR_EQUAL("\\\\.\\pipe\\mcp", config.pipeName, "pipe name");
}

TEST_CASE(cmdline_default) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "empty should succeed with defaults");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "default serial");
    TEST_ASSERT_STR_EQUAL("COM1", config.port, "default COM1");
}

TEST_CASE(cmdline_null) {
    TransportConfig config;
    int result;
    result = ParseCommandLine(NULL, &config);
    TEST_ASSERT_INT_EQUAL(1, result, "NULL should succeed with defaults");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "default serial");
}

TEST_CASE(cmdline_null_config) {
    int result;
    result = ParseCommandLine("/SERIAL:COM1", NULL);
    TEST_ASSERT_INT_EQUAL(0, result, "NULL config should fail");
}

TEST_CASE(cmdline_no_flag) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("some random text", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "no flag keeps defaults");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "default serial");
    TEST_ASSERT_STR_EQUAL("COM1", config.port, "default COM1");
}

TEST_CASE(cmdline_with_exe_prefix) {
    TransportConfig config;
    int result;
    /* GetCommandLineA typically returns: "C:\path\mcp-w32s.exe /SERIAL:COM2" */
    result = ParseCommandLine("C:\\mcp-w32s.exe /SERIAL:COM2", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse with exe prefix");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_SERIAL, config.transport, "transport type");
    TEST_ASSERT_STR_EQUAL("COM2", config.port, "port name");
}

TEST_CASE(cmdline_auto_with_port) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/AUTO:9000", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse successfully");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_TCP, config.transport, "auto prefers tcp");
    TEST_ASSERT_INT_EQUAL(1, config.autodetect, "autodetect enabled");
    TEST_ASSERT_INT_EQUAL(9000, config.tcpPort, "explicit auto port");
}

TEST_CASE(cmdline_auto_default_port) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/AUTO", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "bare /AUTO should parse");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_TCP, config.transport, "auto prefers tcp");
    TEST_ASSERT_INT_EQUAL(1, config.autodetect, "autodetect enabled");
    TEST_ASSERT_INT_EQUAL(DEFAULT_TCP_PORT, config.tcpPort, "default auto port");
}

TEST_CASE(cmdline_bind_address) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/TCP:8932 /BIND:127.0.0.1", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse tcp + bind");
    TEST_ASSERT_INT_EQUAL(TRANSPORT_TCP, config.transport, "transport type");
    TEST_ASSERT_INT_EQUAL(8932, config.tcpPort, "tcp port");
    TEST_ASSERT_STR_EQUAL("127.0.0.1", config.bindAddr, "bind address captured");
}

TEST_CASE(cmdline_bind_defaults_any) {
    TransportConfig config;
    int result;
    result = ParseCommandLine("/TCP:8932", &config);
    TEST_ASSERT_INT_EQUAL(1, result, "should parse");
    TEST_ASSERT_STR_EQUAL("", config.bindAddr, "no /BIND => empty (INADDR_ANY)");
}

/* ========================================================
 * BuildSerialDCB tests
 * ======================================================== */

TEST_CASE(dcb_default_settings) {
    DCB dcb;
    BuildSerialDCB(CBR_115200, &dcb);
    TEST_ASSERT_INT_EQUAL((int)sizeof(DCB), (int)dcb.DCBlength, "DCBlength");
    TEST_ASSERT_INT_EQUAL((int)CBR_115200, (int)dcb.BaudRate, "baud rate");
    TEST_ASSERT_INT_EQUAL(8, (int)dcb.ByteSize, "byte size");
    TEST_ASSERT_INT_EQUAL((int)NOPARITY, (int)dcb.Parity, "parity");
    TEST_ASSERT_INT_EQUAL((int)ONESTOPBIT, (int)dcb.StopBits, "stop bits");
    TEST_ASSERT_INT_EQUAL(TRUE, (int)dcb.fBinary, "binary mode");
}

TEST_CASE(dcb_57600) {
    DCB dcb;
    BuildSerialDCB(CBR_57600, &dcb);
    TEST_ASSERT_INT_EQUAL((int)CBR_57600, (int)dcb.BaudRate, "57600 baud");
    TEST_ASSERT_INT_EQUAL(8, (int)dcb.ByteSize, "byte size");
}

TEST_CASE(dcb_no_flow_control) {
    DCB dcb;
    BuildSerialDCB(CBR_115200, &dcb);
    TEST_ASSERT_INT_EQUAL(FALSE, (int)dcb.fOutxCtsFlow, "no CTS flow");
    TEST_ASSERT_INT_EQUAL(FALSE, (int)dcb.fOutxDsrFlow, "no DSR flow");
    TEST_ASSERT_INT_EQUAL(FALSE, (int)dcb.fOutX, "no XON/XOFF out");
    TEST_ASSERT_INT_EQUAL(FALSE, (int)dcb.fInX, "no XON/XOFF in");
}

TEST_CASE(dcb_null_safe) {
    /* Should not crash */
    BuildSerialDCB(CBR_115200, NULL);
    TEST_ASSERT(1, "did not crash on NULL DCB");
}

/* ========================================================
 * BuildSerialTimeouts tests
 * ======================================================== */

TEST_CASE(timeouts_values) {
    COMMTIMEOUTS t;
    BuildSerialTimeouts(&t);
    /* Interval-only: ReadFile blocks until a burst, idle never ends the
     * session. Total multiplier/constant MUST be 0 (see BuildSerialTimeouts). */
    TEST_ASSERT_INT_EQUAL(50, (int)t.ReadIntervalTimeout, "interval");
    TEST_ASSERT_INT_EQUAL(0, (int)t.ReadTotalTimeoutMultiplier, "no total multiplier");
    TEST_ASSERT_INT_EQUAL(0, (int)t.ReadTotalTimeoutConstant, "no total constant");
    TEST_ASSERT_INT_EQUAL(0, (int)t.WriteTotalTimeoutMultiplier, "write mult");
    TEST_ASSERT_INT_EQUAL(0, (int)t.WriteTotalTimeoutConstant, "write const");
}

TEST_CASE(timeouts_null_safe) {
    BuildSerialTimeouts(NULL);
    TEST_ASSERT(1, "did not crash on NULL timeouts");
}

/* ========================================================
 * ProcessBuffer tests
 * ======================================================== */

TEST_CASE(buffer_single_line) {
    LineBuffer buf;
    int lines;
    memset(&buf, 0, sizeof(buf));
    reset_capture();

    lines = ProcessBuffer(&buf, "hello\n", 6, capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(1, lines, "one line processed");
    TEST_ASSERT_INT_EQUAL(1, g_captured_count, "one line captured");
    TEST_ASSERT_STR_EQUAL("hello", g_captured_lines[0], "line content");
}

TEST_CASE(buffer_two_lines) {
    LineBuffer buf;
    int lines;
    memset(&buf, 0, sizeof(buf));
    reset_capture();

    lines = ProcessBuffer(&buf, "one\ntwo\n", 8, capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(2, lines, "two lines processed");
    TEST_ASSERT_INT_EQUAL(2, g_captured_count, "two lines captured");
    TEST_ASSERT_STR_EQUAL("one", g_captured_lines[0], "first line");
    TEST_ASSERT_STR_EQUAL("two", g_captured_lines[1], "second line");
}

TEST_CASE(buffer_partial_line) {
    LineBuffer buf;
    int lines;
    memset(&buf, 0, sizeof(buf));
    reset_capture();

    /* First chunk: partial line */
    lines = ProcessBuffer(&buf, "hel", 3, capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(0, lines, "no complete line yet");
    TEST_ASSERT_INT_EQUAL(0, g_captured_count, "nothing captured");

    /* Second chunk: rest of line + newline */
    lines = ProcessBuffer(&buf, "lo\n", 3, capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(1, lines, "one line completed");
    TEST_ASSERT_INT_EQUAL(1, g_captured_count, "one line captured");
    TEST_ASSERT_STR_EQUAL("hello", g_captured_lines[0], "reassembled line");
}

TEST_CASE(buffer_empty_line) {
    LineBuffer buf;
    int lines;
    memset(&buf, 0, sizeof(buf));
    reset_capture();

    lines = ProcessBuffer(&buf, "\n", 1, capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(1, lines, "one line (empty)");
    TEST_ASSERT_INT_EQUAL(1, g_captured_count, "captured empty line");
    TEST_ASSERT_STR_EQUAL("", g_captured_lines[0], "empty content");
}

TEST_CASE(buffer_no_input) {
    LineBuffer buf;
    int lines;
    memset(&buf, 0, sizeof(buf));
    reset_capture();

    lines = ProcessBuffer(&buf, "", 0, capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(0, lines, "no lines from empty input");
    TEST_ASSERT_INT_EQUAL(0, g_captured_count, "nothing captured");
}

TEST_CASE(buffer_null_handler) {
    LineBuffer buf;
    int lines;
    memset(&buf, 0, sizeof(buf));

    /* Should not crash with NULL handler */
    lines = ProcessBuffer(&buf, "test\n", 5, NULL, NULL);
    TEST_ASSERT_INT_EQUAL(1, lines, "line counted even with NULL handler");
}

TEST_CASE(buffer_json_command) {
    LineBuffer buf;
    int lines;
    const char *json;
    memset(&buf, 0, sizeof(buf));
    reset_capture();

    json = "{\"cmd\":\"exec\",\"id\":\"1\",\"line\":\"dir\"}\n";
    lines = ProcessBuffer(&buf, json, (int)strlen(json), capture_handler,
                          NULL);
    TEST_ASSERT_INT_EQUAL(1, lines, "one JSON line");
    TEST_ASSERT_STR_EQUAL(
        "{\"cmd\":\"exec\",\"id\":\"1\",\"line\":\"dir\"}",
        g_captured_lines[0], "JSON preserved");
}

/* ========================================================
 * ProcessCommand tests (dispatch + response bytes)
 *
 * With the mock transport backend we can assert the EXACT response
 * bytes ProcessCommand writes - something the old HANDLE-based tests
 * could not do. The mock captures every byte written via the vtable.
 * ======================================================== */

/* Helper: run one command through ProcessCommand into a fresh mock and
 * NUL-terminate the captured output for string assertions. */
static void run_command(const char *line, MockTransport *m, char *out, int outSize)
{
    int n;
    MockTransportInit(m, NULL, 0);
    ProcessCommand(line, &m->t);
    n = m->outLen;
    if (n > outSize - 1) {
        n = outSize - 1;
    }
    memcpy(out, m->out, (size_t)n);
    out[n] = '\0';
}

TEST_CASE(dispatch_echo_response) {
    MockTransport m;
    char out[512];
    run_command("{\"cmd\":\"echo\",\"id\":\"1\",\"line\":\"hello\"}", &m,
                out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"id\":\"1\"") != NULL, "id echoed");
    TEST_ASSERT(strstr(out, "\"status\":\"ok\"") != NULL, "status ok");
    TEST_ASSERT(strstr(out, "hello") != NULL, "payload echoed");
    TEST_ASSERT(m.outLen > 0, "bytes were written via the transport");
}

TEST_CASE(dispatch_unknown_response) {
    MockTransport m;
    char out[512];
    run_command("{\"cmd\":\"reboot\",\"id\":\"99\"}", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"error\"") != NULL, "error status");
    TEST_ASSERT(strstr(out, "unknown command") != NULL, "unknown reason");
}

TEST_CASE(dispatch_malformed_json) {
    MockTransport m;
    char out[512];
    run_command("{bad json", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "invalid JSON") != NULL, "invalid JSON reported");
}

TEST_CASE(dispatch_known_commands) {
    /* All known commands handled without crash, via the transport. */
    MockTransport m;
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"1\",\"line\":\"dir\"}", &m.t);
    ProcessCommand("{\"cmd\":\"read\",\"id\":\"2\",\"path\":\"C:\\\\test\"}", &m.t);
    ProcessCommand("{\"cmd\":\"write\",\"id\":\"3\",\"path\":\"C:\\\\f\","
                   "\"data\":\"AA==\"}", &m.t);
    ProcessCommand("{\"cmd\":\"list\",\"id\":\"4\",\"path\":\"C:\\\\\"}", &m.t);
    ProcessCommand("{\"cmd\":\"delete\",\"id\":\"5\",\"path\":\"C:\\\\old\"}", &m.t);
    TEST_ASSERT(1, "all known commands handled without crash");
}

TEST_CASE(dispatch_empty_line) {
    /* Should not crash on empty input; NULL transport also tolerated. */
    ProcessCommand("", NULL);
    ProcessCommand(NULL, NULL);
    TEST_ASSERT(1, "empty/null input handled without crash");
}


/* ========================================================
 * Phase 4 exec integration (full JSON -> ProcessCommand ->
 * response shape). Obligations (tests/OBLIGATIONS-PHASE4.md):
 * rule-success.ExecCommand, rule-success.ExecSuccess,
 * rule-success.ExecRejectedResponse, rule-success.GateMiss,
 * rule-success.GateBypassedByUnsafeRequest (+unsafe_used field),
 * rule-success.GateBuiltinHit (auto-route, exec_method shell),
 * rule-success.ExecRequestBusy / OrphanReaped (busy detail+reap),
 * rule-success.ExecStdinTooLarge, invariant.ShellRoutingReported.
 * ======================================================== */

static Catalog *load_test_catalog(void)
{
    static const char *candidates[] = {
        "catalog\\win32-commands.json",
        "..\\catalog\\win32-commands.json",
        "..\\..\\catalog\\win32-commands.json"
    };
    Catalog *cat;
    char err[160];
    int i;

    for (i = 0; i < 3; i++) {
        cat = NULL;
        if (CatalogLoad(candidates[i], &cat, err, (int)sizeof(err))) {
            return cat;
        }
    }
    return NULL;
}

TEST_CASE(exec_integration_full_response) {
    /* exec happy path: every Phase 4 response key present.
     * unsafe:true so the test is catalog-independent; asserts the
     * unsafe_used response field (decision 6). */
    MockTransport m;
    ExecConfigure(NULL, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e1\","
                   "\"argv\":[\"cmd\",\"/c\",\"echo\",\"hi\"],"
                   "\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"id\":\"e1\"") != NULL, "id echoed");
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "status ok");
    TEST_ASSERT(strstr(m.out, "\"exit_code\":0") != NULL, "exit 0");
    TEST_ASSERT(strstr(m.out, "\"stdout_b64\":\"aGkNCg==\"") != NULL,
                "stdout 'hi\\r\\n' base64");
    TEST_ASSERT(strstr(m.out, "\"stderr_b64\":\"\"") != NULL, "stderr empty");
    TEST_ASSERT(strstr(m.out, "\"stdout_truncated\":false") != NULL,
                "stdout not truncated");
    TEST_ASSERT(strstr(m.out, "\"duration_ms\":") != NULL, "duration present");
    TEST_ASSERT(strstr(m.out, "\"exec_method\":\"direct\"") != NULL,
                "direct exec");
    TEST_ASSERT(strstr(m.out, "\"binary_type\":\"pe32\"") != NULL,
                "cmd.exe is pe32");
    TEST_ASSERT(strstr(m.out, "\"killed_by\":\"\"") != NULL, "not killed");
    TEST_ASSERT(strstr(m.out, "\"unsafe_used\":true") != NULL,
                "unsafe_used reported in the response, not stderr");
}

TEST_CASE(exec_catalog_miss_rejected) {
    /* enforced catalog + uncatalogued command -> command not in
     * catalog (rule GateMiss). */
    MockTransport m;
    Catalog *cat;
    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e2\","
                   "\"argv\":[\"nonexistent_xyz\"]}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL, "error");
    TEST_ASSERT(strstr(m.out, "command not in catalog") != NULL,
                "catalog miss reason");
    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

TEST_CASE(exec_unsafe_bypasses_catalog) {
    /* per-request unsafe bypasses the whitelist for one exec
     * (rule GateBypassedByUnsafeRequest). */
    MockTransport m;
    Catalog *cat;
    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e3\","
                   "\"argv\":[\"cmd\",\"/c\",\"echo\",\"ok\"],"
                   "\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL,
                "uncatalogued cmd runs with unsafe");
    TEST_ASSERT(strstr(m.out, "\"unsafe_used\":true") != NULL,
                "bypass reported");
    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

TEST_CASE(exec_builtin_autoroutes_via_shell) {
    /* catalogued shell built-in auto-routes via the era shell even
     * with shell:false (decision 3; invariant ShellRoutingReported). */
    MockTransport m;
    Catalog *cat;
    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e4\","
                   "\"argv\":[\"ver\"],\"shell\":false}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "ver runs");
    TEST_ASSERT(strstr(m.out, "\"exec_method\":\"shell\"") != NULL,
                "exec_method reports shell routing");
    TEST_ASSERT(strstr(m.out, "\"binary_type\":\"shell-builtin\"") != NULL,
                "builtin classified without file read");
    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

TEST_CASE(exec_busy_carries_detail_then_reaps) {
    /* still_active orphan blocks exec AND ptyExec with informative
     * busy; once the orphan exits the next request proceeds
     * (rules ExecRequestBusy / OrphanReaped, decisions 9+10). */
    MockTransport m;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    static char childCmd[64];
    int ok;

    ExecConfigure(NULL, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    lstrcpyA(childCmd, "cmd /c ping -n 30 127.0.0.1");
    ok = CreateProcessA(NULL, childCmd, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si, &pi);
    TEST_ASSERT(ok, "long-running child spawns");
    CloseHandle(pi.hThread);
    ExecInjectOrphanForTest(pi.hProcess, GetTickCount(),
                            "legacy16.exe /batch");

    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e5\","
                   "\"argv\":[\"cmd\",\"/c\",\"echo\",\"x\"],"
                   "\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"error\":\"busy\"") != NULL, "busy");
    TEST_ASSERT(strstr(m.out, "\"blocking_cmd_line\":\"legacy16.exe /batch\"")
                != NULL, "busy carries the blocking cmd_line");
    TEST_ASSERT(strstr(m.out, "\"elapsed_ms\":") != NULL,
                "busy carries elapsed ms");

    /* ptyExec shares the busy domain (decision 10) */
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"ptyExec\",\"id\":\"e6\","
                   "\"argv\":[\"cmd\"],\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"error\":\"busy\"") != NULL,
                "ptyExec blocked by the same orphan");

    /* Reap: kill the orphan; the next request proceeds. */
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e7\","
                   "\"argv\":[\"cmd\",\"/c\",\"echo\",\"y\"],"
                   "\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL,
                "request proceeds after implicit reap");
}

TEST_CASE(exec_stdin_too_large_rejected) {
    /* decoded stdin above the 4096 one-pipe-buffer cap is an error,
     * not a clamp (rule ExecStdinTooLarge, config stdin_max). */
    MockTransport m;
    static unsigned char raw[4097];
    static char b64[8192];
    static char json[16384];
    int i;
    for (i = 0; i < 4097; i++) {
        raw[i] = (unsigned char)'a';
    }
    TEST_ASSERT(Base64Encode(raw, 4097, b64, (int)sizeof(b64)) > 0,
                "encode oversize stdin");
    ExecConfigure(NULL, 0);
    json[0] = '\0';
    lstrcatA(json, "{\"cmd\":\"exec\",\"id\":\"e8\","
                   "\"argv\":[\"cmd\",\"/c\",\"more\"],"
                   "\"unsafe\":true,\"stdin_b64\":\"");
    lstrcatA(json, b64);
    lstrcatA(json, "\"}");
    MockTransportInit(&m, NULL, 0);
    ProcessCommand(json, &m.t);
    TEST_ASSERT(strstr(m.out, "stdin too large") != NULL,
                "oversize stdin rejected");
}

TEST_CASE(exec_invalid_stdin_b64_rejected) {
    MockTransport m;
    ExecConfigure(NULL, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"e9\","
                   "\"argv\":[\"cmd\"],\"unsafe\":true,"
                   "\"stdin_b64\":\"!!notb64!!\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "invalid base64") != NULL,
                "invalid stdin_b64 rejected");
}

TEST_CASE(ptyexec_capability_absent_error) {
    /* ptyExec on a host without ConPTY -> explicit error
     * (rule PtyUnavailable; rule PtyExecCommand dispatches). */
    MockTransport m;
    FeatForceFallback(FEAT_FORCE_NO_PTY);
    ExecConfigure(NULL, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"ptyExec\",\"id\":\"p1\","
                   "\"argv\":[\"cmd\"],\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "pty not available on this Windows") != NULL,
                "capability-absent error");
    FeatInit(); /* restore */
}

/* ========================================================
 * Main - Run all tests
 * ======================================================== */

int main(void)
{
    printf("Serial + Main Loop Tests\n");
    printf("========================================\n");

    printf("\nParseCommandLine:\n");
    RUN_TEST(cmdline_serial_com1);
    RUN_TEST(cmdline_serial_com2);
    RUN_TEST(cmdline_serial_lowercase);
    RUN_TEST(cmdline_tcp);
    RUN_TEST(cmdline_tcp_invalid_port);
    RUN_TEST(cmdline_pipe);
    RUN_TEST(cmdline_default);
    RUN_TEST(cmdline_null);
    RUN_TEST(cmdline_null_config);
    RUN_TEST(cmdline_no_flag);
    RUN_TEST(cmdline_with_exe_prefix);
    RUN_TEST(cmdline_auto_with_port);
    RUN_TEST(cmdline_auto_default_port);
    RUN_TEST(cmdline_bind_address);
    RUN_TEST(cmdline_bind_defaults_any);

    printf("\nBuildSerialDCB:\n");
    RUN_TEST(dcb_default_settings);
    RUN_TEST(dcb_57600);
    RUN_TEST(dcb_no_flow_control);
    RUN_TEST(dcb_null_safe);

    printf("\nBuildSerialTimeouts:\n");
    RUN_TEST(timeouts_values);
    RUN_TEST(timeouts_null_safe);

    printf("\nProcessBuffer:\n");
    RUN_TEST(buffer_single_line);
    RUN_TEST(buffer_two_lines);
    RUN_TEST(buffer_partial_line);
    RUN_TEST(buffer_empty_line);
    RUN_TEST(buffer_no_input);
    RUN_TEST(buffer_null_handler);
    RUN_TEST(buffer_json_command);

    printf("\nProcessCommand (dispatch + response bytes):\n");
    RUN_TEST(dispatch_echo_response);
    RUN_TEST(dispatch_unknown_response);
    RUN_TEST(dispatch_malformed_json);
    RUN_TEST(dispatch_known_commands);
    RUN_TEST(dispatch_empty_line);

    printf("\nPhase 4 exec integration:\n");
    FeatInit();
    RUN_TEST(exec_integration_full_response);
    RUN_TEST(exec_catalog_miss_rejected);
    RUN_TEST(exec_unsafe_bypasses_catalog);
    RUN_TEST(exec_builtin_autoroutes_via_shell);
    RUN_TEST(exec_busy_carries_detail_then_reaps);
    RUN_TEST(exec_stdin_too_large_rejected);
    RUN_TEST(exec_invalid_stdin_b64_rejected);
    RUN_TEST(ptyexec_capability_absent_error);

    print_test_summary();
    return g_tests_failed;
}
