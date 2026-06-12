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
#include "ready.h"
#include "mem_ops.h"
#include "encoding.h"
#include "audit.h"
#include "uart.h"

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

/* exec dispatcher hooks (mcp-w32s.c, TEST_BUILD) */
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
    ProcessCommand("{\"cmd\":\"copy\",\"id\":\"6\",\"path\":\"C:\\\\a\","
                   "\"dest\":\"C:\\\\b\"}", &m.t);
    ProcessCommand("{\"cmd\":\"move\",\"id\":\"7\",\"path\":\"C:\\\\a\","
                   "\"dest\":\"C:\\\\b\"}", &m.t);
    ProcessCommand("{\"cmd\":\"mkdir\",\"id\":\"8\",\"path\":\"C:\\\\d\"}", &m.t);
    ProcessCommand("{\"cmd\":\"rmdir\",\"id\":\"9\",\"path\":\"C:\\\\d\"}", &m.t);
    TEST_ASSERT(1, "all known commands handled without crash");
}

TEST_CASE(dispatch_empty_line) {
    /* Should not crash on empty input; NULL transport also tolerated. */
    ProcessCommand("", NULL);
    ProcessCommand(NULL, NULL);
    TEST_ASSERT(1, "empty/null input handled without crash");
}

/* ========================================================
 * File-management wire round-trips (full JSON -> ProcessCommand
 * -> response envelope). Obligations (tests/OBLIGATIONS-5.1.md):
 * rule-success.{Copy,Move,Mkdir,Rmdir}Command (+ .failure.1 and
 * rule-entity-creation.*.1 - dispatch reaches the handler, not
 * "unknown command"), rule-success.{Copy,Move,Mkdir,Rmdir}Success
 * (the copied/moved/created/removed envelopes),
 * rule-success.{Copy,Move,Mkdir,Rmdir}Error (representative pinned
 * reasons riding the error envelope).
 * ======================================================== */

TEST_CASE(dispatch_copy_ok_envelope) {
    MockTransport m;
    char out[512];
    DeleteFileA("ser51_src.txt");
    DeleteFileA("ser51_dst.txt");
    run_command("{\"cmd\":\"write\",\"id\":\"w\",\"path\":\"ser51_src.txt\","
                "\"data\":\"aGVsbG8=\"}", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"ok\"") != NULL, "fixture written");
    run_command("{\"cmd\":\"copy\",\"id\":\"c1\",\"path\":\"ser51_src.txt\","
                "\"dest\":\"ser51_dst.txt\"}", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"id\":\"c1\"") != NULL, "id correlated");
    TEST_ASSERT(strstr(out, "\"status\":\"ok\"") != NULL, "status ok");
    TEST_ASSERT(strstr(out, "\"message\":\"copied\"") != NULL, "copied envelope");
    run_command("{\"cmd\":\"read\",\"id\":\"r\",\"path\":\"ser51_dst.txt\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "aGVsbG8=") != NULL, "dest carries source content");
    DeleteFileA("ser51_src.txt");
    DeleteFileA("ser51_dst.txt");
}

TEST_CASE(dispatch_copy_dest_exists_envelope) {
    /* The fail-if-exists pin observed at the wire. */
    MockTransport m;
    char out[512];
    run_command("{\"cmd\":\"write\",\"id\":\"w1\",\"path\":\"ser51_src.txt\","
                "\"data\":\"aGVsbG8=\"}", &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"write\",\"id\":\"w2\",\"path\":\"ser51_dst.txt\","
                "\"data\":\"d29ybGQ=\"}", &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"copy\",\"id\":\"c2\",\"path\":\"ser51_src.txt\","
                "\"dest\":\"ser51_dst.txt\"}", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"error\"") != NULL, "error status");
    TEST_ASSERT(strstr(out, "file exists") != NULL, "file exists reason");
    run_command("{\"cmd\":\"read\",\"id\":\"r2\",\"path\":\"ser51_dst.txt\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "d29ybGQ=") != NULL, "dest content untouched");
    DeleteFileA("ser51_src.txt");
    DeleteFileA("ser51_dst.txt");
}

TEST_CASE(dispatch_move_ok_envelope) {
    MockTransport m;
    char out[512];
    DeleteFileA("ser51_mv.txt");
    DeleteFileA("ser51_mvd.txt");
    run_command("{\"cmd\":\"write\",\"id\":\"w\",\"path\":\"ser51_mv.txt\","
                "\"data\":\"aGVsbG8=\"}", &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"move\",\"id\":\"m1\",\"path\":\"ser51_mv.txt\","
                "\"dest\":\"ser51_mvd.txt\"}", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"id\":\"m1\"") != NULL, "id correlated");
    TEST_ASSERT(strstr(out, "\"message\":\"moved\"") != NULL, "moved envelope");
    run_command("{\"cmd\":\"read\",\"id\":\"r\",\"path\":\"ser51_mv.txt\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"error\"") != NULL, "source gone");
    run_command("{\"cmd\":\"read\",\"id\":\"r2\",\"path\":\"ser51_mvd.txt\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "aGVsbG8=") != NULL, "dest carries content");
    DeleteFileA("ser51_mvd.txt");
}

TEST_CASE(dispatch_move_missing_envelope) {
    MockTransport m;
    char out[512];
    DeleteFileA("ser51_none.txt");
    run_command("{\"cmd\":\"move\",\"id\":\"m2\",\"path\":\"ser51_none.txt\","
                "\"dest\":\"ser51_x.txt\"}", &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"error\"") != NULL, "error status");
    TEST_ASSERT(strstr(out, "file not found") != NULL, "file not found reason");
}

TEST_CASE(dispatch_mkdir_ok_envelope) {
    MockTransport m;
    char out[512];
    RemoveDirectoryA("ser51_dir");
    run_command("{\"cmd\":\"mkdir\",\"id\":\"k1\",\"path\":\"ser51_dir\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"id\":\"k1\"") != NULL, "id correlated");
    TEST_ASSERT(strstr(out, "\"message\":\"created\"") != NULL, "created envelope");
    RemoveDirectoryA("ser51_dir");
}

TEST_CASE(dispatch_mkdir_exists_envelope) {
    MockTransport m;
    char out[512];
    RemoveDirectoryA("ser51_dir");
    run_command("{\"cmd\":\"mkdir\",\"id\":\"k1\",\"path\":\"ser51_dir\"}",
                &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"mkdir\",\"id\":\"k2\",\"path\":\"ser51_dir\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"error\"") != NULL, "error status");
    TEST_ASSERT(strstr(out, "directory exists") != NULL, "directory exists reason");
    RemoveDirectoryA("ser51_dir");
}

TEST_CASE(dispatch_rmdir_ok_envelope) {
    MockTransport m;
    char out[512];
    run_command("{\"cmd\":\"mkdir\",\"id\":\"k\",\"path\":\"ser51_rd\"}",
                &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"rmdir\",\"id\":\"d1\",\"path\":\"ser51_rd\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"id\":\"d1\"") != NULL, "id correlated");
    TEST_ASSERT(strstr(out, "\"message\":\"removed\"") != NULL, "removed envelope");
    run_command("{\"cmd\":\"rmdir\",\"id\":\"d2\",\"path\":\"ser51_rd\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "directory not found") != NULL,
                "second rmdir reports directory not found");
}

TEST_CASE(dispatch_rmdir_nonempty_envelope) {
    /* The non-empty refusal pin observed at the wire. */
    MockTransport m;
    char out[512];
    run_command("{\"cmd\":\"mkdir\",\"id\":\"k\",\"path\":\"ser51_ne\"}",
                &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"write\",\"id\":\"w\",\"path\":\"ser51_ne\\\\f.txt\","
                "\"data\":\"AA==\"}", &m, out, (int)sizeof(out));
    run_command("{\"cmd\":\"rmdir\",\"id\":\"d3\",\"path\":\"ser51_ne\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"error\"") != NULL, "error status");
    TEST_ASSERT(strstr(out, "directory not empty") != NULL,
                "directory not empty reason");
    run_command("{\"cmd\":\"read\",\"id\":\"r\",\"path\":\"ser51_ne\\\\f.txt\"}",
                &m, out, (int)sizeof(out));
    TEST_ASSERT(strstr(out, "\"status\":\"ok\"") != NULL, "contents untouched");
    DeleteFileA("ser51_ne\\f.txt");
    RemoveDirectoryA("ser51_ne");
}


/* ========================================================
 * exec integration (full JSON -> ProcessCommand ->
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
    /* exec happy path: every response key present.
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

TEST_CASE(exec_builtin_positional_metachar_neutralised) {
    /* Catalog-gate bypass regression (adversarial review of PR #10):
     * a cmd metacharacter in a positional arg of a catalogued builtin
     * must be caret-escaped, never reach the shell as a separator.
     * "echo" is a catalogued builtin; "x&ver" is an allowed positional
     * with NO space, so ArgvJoin emits it bare. Unescaped, the shell
     * runs `echo x` then `ver` (the uncatalogued ver banner, which
     * contains "Windows"). Escaped, it prints the literal "x&ver" -
     * which contains no banner. We decode stdout_b64 and assert the
     * banner is absent: that proves the second command did not run.
     * Obligation: catalog.allium ShellTailNeutralised. */
    MockTransport m;
    Catalog *cat;
    const char *b64Start;
    static unsigned char decoded[8192];
    int dn;
    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"sec1\","
                   "\"argv\":[\"echo\",\"x&ver\"]}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL,
                "escaped builtin still runs");

    /* Decode the stdout_b64 field and check for the ver banner. */
    b64Start = strstr(m.out, "\"stdout_b64\":\"");
    TEST_ASSERT(b64Start != NULL, "response carries stdout_b64");
    if (b64Start != NULL) {
        char b64[8192];
        const char *p;
        int n;
        b64Start += lstrlenA("\"stdout_b64\":\"");
        n = 0;
        for (p = b64Start; *p != '\0' && *p != '"' && n < 8191; p++) {
            b64[n++] = *p;
        }
        b64[n] = '\0';
        dn = Base64Decode(b64, decoded, (int)sizeof(decoded) - 1);
        if (dn < 0) {
            dn = 0;
        }
        decoded[dn] = '\0';
        TEST_ASSERT(strstr((char *)decoded, "Windows") == NULL &&
                    strstr((char *)decoded, "Microsoft") == NULL,
                    "the chained 'ver' did NOT run (metachar escaped)");
    }
    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

/*
 * wellformed_utf8 - Table 3-7 ("Well-Formed UTF-8 Byte Sequences") validator.
 * Returns 1 iff every byte of b[0..n) belongs to a well-formed sequence (no
 * lone/over-long/surrogate/truncated forms). Used to pin NeverEmitInvalidUtf8
 * at the exec-output transcode seam.
 */
static int wellformed_utf8(const unsigned char *b, int n)
{
    int i;
    i = 0;
    while (i < n) {
        unsigned char c;
        int len;
        int lo;
        int hi;
        int k;
        c = b[i];
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF)      { len = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xE0)              { len = 3; lo = 0xA0; hi = 0xBF; }
        else if (c >= 0xE1 && c <= 0xEC) { len = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xED)              { len = 3; lo = 0x80; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { len = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF0)              { len = 4; lo = 0x90; hi = 0xBF; }
        else if (c >= 0xF1 && c <= 0xF3) { len = 4; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF4)              { len = 4; lo = 0x80; hi = 0x8F; }
        else { return 0; }   /* C0/C1, F5-FF, or a stray continuation byte */
        if (i + len > n) { return 0; }                 /* truncated tail */
        if (b[i + 1] < lo || b[i + 1] > hi) { return 0; }
        for (k = 2; k < len; k++) {
            if (b[i + k] < 0x80 || b[i + k] > 0xBF) { return 0; }
        }
        i += len;
    }
    return 1;
}

/*
 * Obligation: encoding.allium OutputConverted + NeverEmitInvalidUtf8, the
 * exec-output transcode seam (OBLIGATIONS-5.4.md). A child emitting a non-ASCII
 * byte comes back as WELL-FORMED UTF-8 on the wire. The exact glyph is
 * host-dependent (the child's console code page), but UTF-8 well-formedness is
 * the invariant: without the transcode the raw lone high byte would be invalid
 * UTF-8 and this fails. Host-tolerant (runs under native WSL and Wine).
 */
TEST_CASE(exec_output_is_valid_utf8) {
    MockTransport m;
    const char *b64Start;
    static unsigned char decoded[8192];
    int dn;

    ExecConfigure(NULL, 0);
    MockTransportInit(&m, NULL, 0);
    /* echo a UTF-8 U+00E9 (LATIN SMALL LETTER E WITH ACUTE). */
    ProcessCommand("{\"cmd\":\"exec\",\"id\":\"u1\","
                   "\"argv\":[\"cmd\",\"/c\",\"echo\",\"\xC3\xA9\"],"
                   "\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "exec ran");

    b64Start = strstr(m.out, "\"stdout_b64\":\"");
    TEST_ASSERT(b64Start != NULL, "response carries stdout_b64");
    if (b64Start != NULL) {
        char b64[8192];
        const char *p;
        int n;
        b64Start += lstrlenA("\"stdout_b64\":\"");
        n = 0;
        for (p = b64Start; *p != '\0' && *p != '"' && n < 8191; p++) {
            b64[n++] = *p;
        }
        b64[n] = '\0';
        dn = Base64Decode(b64, decoded, (int)sizeof(decoded));
        if (dn < 0) {
            dn = 0;
        }
        TEST_ASSERT(wellformed_utf8(decoded, dn),
                    "exec stdout is well-formed UTF-8");
    }
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

/*
 * The ptyExec transcode seam: ConPTY output is transcoded to UTF-8, so the
 * response declares output_kind:"utf8". output_kind is an unmodelled wire
 * detail (no structural obligation - OBLIGATIONS-5.4.md notes the planned
 * mcp-protocol edit was a no-op); the wire change is documented in
 * wire-contract.allium and the underlying safety is NeverEmitInvalidUtf8. This
 * pins the observable contract change. ConPTY is Win10 1809+; skip-with-reason
 * where absent (e.g. Wine CI) - runner-verified on a ConPTY host.
 */
TEST_CASE(ptyexec_output_kind_utf8) {
    MockTransport m;

    if (!g_features.has_create_pseudo_console) {
        printf("    [skip] no ConPTY on this host\n");
        return;
    }
    ExecConfigure(NULL, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"ptyExec\",\"id\":\"pk\","
                   "\"argv\":[\"cmd\",\"/c\",\"echo\",\"hi\"],"
                   "\"unsafe\":true}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"output_kind\":\"utf8\"") != NULL,
                "ptyExec declares output_kind utf8");
}

/* ========================================================
 * Ready message: features.toolchains array
 * (spec: wire-contract.allium ReadyShape; toolchains.allium
 * ToolchainDetected). BuildReadyMessage embeds the detected set inside
 * the features object as a `toolchains` array - empty when none.
 * ======================================================== */

TEST_CASE(ready_lists_detected_toolchains) {
    char json[2048];
    ToolchainSet set;
    int len;

    /* No toolchain installed -> the array is present and EMPTY (not absent,
     * not null): the bridge always reads features.toolchains. */
    memset(&set, 0, sizeof(set));
    len = BuildReadyMessage("tcp", NULL, &set, json, (int)sizeof(json));
    TEST_ASSERT(len > 0, "ready message built");
    TEST_ASSERT(strstr(json, "\"toolchains\":[]") != NULL,
                "empty set -> empty toolchains array inside features");

    /* A NULL set is treated the same (defensive). */
    len = BuildReadyMessage("tcp", NULL, NULL, json, (int)sizeof(json));
    TEST_ASSERT(len > 0, "ready message built (null set)");
    TEST_ASSERT(strstr(json, "\"toolchains\":[]") != NULL,
                "null set -> empty toolchains array");

    /* One detected toolchain -> one {vendor,command,version} object. */
    memset(&set, 0, sizeof(set));
    lstrcpyA(set.items[0].vendor, "Microsoft");
    lstrcpyA(set.items[0].command, "cl");
    lstrcpyA(set.items[0].version, "12.00.8804");
    set.count = 1;
    len = BuildReadyMessage("tcp", NULL, &set, json, (int)sizeof(json));
    TEST_ASSERT(len > 0, "ready message built (one toolchain)");
    TEST_ASSERT(strstr(json,
        "\"toolchains\":[{\"vendor\":\"Microsoft\",\"command\":\"cl\","
        "\"version\":\"12.00.8804\"}]") != NULL,
        "detected toolchain appears as a features.toolchains entry");
    /* The array sits inside the features object (before its closing brace). */
    TEST_ASSERT(strstr(json, "\"features\":{") != NULL, "features object present");
}

/* ========================================================
 * Memory peek/poke wire dispatch (spec: memory-ops.allium).
 * The module-level RPM/WPM round-trip + the safety guards live in
 * test_mem_ops.c; these prove the ProcessCommand SEAM - field parsing,
 * verb routing, the catalog/arm gates at the wire, and the JSON response
 * builders in HandleMem.
 * ======================================================== */

/* Copy a JSON string value for `key` out of `json` into `out`. */
static int extract_json_str(const char *json, const char *key,
                            char *out, int outSize)
{
    const char *p;
    char pat[40];
    int i;

    wsprintfA(pat, "\"%s\":\"", key);
    p = strstr(json, pat);
    if (p == NULL) {
        return 0;
    }
    p += lstrlenA(pat);
    for (i = 0; i < outSize - 1 && *p != '\0' && *p != '"'; i++) {
        out[i] = *p++;
    }
    out[i] = '\0';
    return 1;
}

TEST_CASE(mem_ready_carries_tier) {
    char json[2048];
    char want[40];
    ToolchainSet set;
    int len;

    FeatInit();
    memset(&set, 0, sizeof(set));
    len = BuildReadyMessage("tcp", NULL, &set, json, (int)sizeof(json));
    TEST_ASSERT(len > 0, "ready built");
    /* features.mem is always present and is the OS-family tier (host-tolerant:
     * the exact value follows g_features, "process" on the NT dev host/CI). */
    wsprintfA(want, "\"mem\":\"%s\"", MemTierName(MemTierCurrent()));
    TEST_ASSERT(strstr(json, want) != NULL, "features.mem carries the tier");
}

TEST_CASE(encoding_ready_carries_tag) {
    char json[2048];
    char want[48];
    ToolchainSet set;
    int len;

    FeatInit();
    memset(&set, 0, sizeof(set));
    len = BuildReadyMessage("tcp", NULL, &set, json, (int)sizeof(json));
    TEST_ASSERT(len > 0, "ready built");
    /* features.encoding is always present and is the OS-family provenance tag
     * (host-tolerant: the exact value follows the tier, "utf8_via_w" on the NT
     * dev host/CI where the -W uplift is live). Informational only. */
    wsprintfA(want, "\"encoding\":\"%s\"", EncProvenanceTag());
    TEST_ASSERT(strstr(json, want) != NULL, "features.encoding carries the tag");
}

TEST_CASE(mem_spawn_retain_uncatalogued_refused) {
    /* SAFETY PIN #5 at the wire: an enforced catalog + an uncatalogued
     * command -> spawnRetain refused (no launch-anything bypass). */
    MockTransport m;
    Catalog *cat;

    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"spawnRetain\",\"id\":\"s9\","
                   "\"argv\":[\"nonexistent_xyz\"]}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL,
                "uncatalogued spawnRetain refused");
    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

TEST_CASE(mem_poke_unarmed_refused) {
    /* SAFETY PIN #7 (device half) at the wire: with the /ALLOWMEMWRITE arm
     * absent, a poke is refused regardless of the request - the wire arm
     * binds every client. (The arm is checked before the target, so no real
     * process is needed.) */
    MockTransport m;

    AuditConfigure(0, NULL);   /* disarmed */
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"poke\",\"id\":\"p9\",\"mem_token\":\"m1-1\","
                   "\"mem_addr\":\"0x1000\",\"data\":\"QUJD\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL,
                "unarmed poke refused at the wire");
    TEST_ASSERT(strstr(m.out, "arm") != NULL, "reason names the arm");
}

TEST_CASE(mem_peek_bad_token_errors) {
    /* The peek dispatch parses mem_addr/mem_len and reaches MemPeek, which
     * rejects a forged token on the process tier (RetainedTokenValid). A
     * malformed address is a distinct, earlier rejection. */
    MockTransport m;

    MemReleaseAll();
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"peek\",\"id\":\"k1\",\"mem_token\":\"forged\","
                   "\"mem_addr\":\"0x1000\",\"mem_len\":\"8\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL,
                "a forged token is rejected");

    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"peek\",\"id\":\"k2\",\"mem_token\":\"forged\","
                   "\"mem_addr\":\"zzz\",\"mem_len\":\"8\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "invalid address") != NULL,
                "a malformed address is rejected by the parse guard");
}

TEST_CASE(mem_process_tier_requires_token) {
    /* Gate-bypass guard: on the NT process tier a token-LESS peek/poke must be
     * REFUSED - it must never fall through to a local read/write of the
     * device's OWN memory (the spawn-retain table is the sole process-tier
     * target; process = null is a pre-NT concept). */
    MockTransport m;

    FeatInit();
    if (!g_features.is_nt) {
        printf("(skip: process-tier token rule is NT-only) ");
        return;
    }
    MemReleaseAll();

    /* peek with no mem_token -> refused, not a self-read. */
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"peek\",\"id\":\"t1\","
                   "\"mem_addr\":\"0x10000\",\"mem_len\":\"8\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL,
                "token-less peek refused on the process tier");
    TEST_ASSERT(strstr(m.out, "token") != NULL, "reason names the token");
    TEST_ASSERT(strstr(m.out, "\"data_b64\"") == NULL,
                "no device memory was read");

    /* poke with no mem_token, even when armed -> refused, not a self-write. */
    AuditConfigure(1, NULL);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"poke\",\"id\":\"t2\","
                   "\"mem_addr\":\"0x10000\",\"data\":\"QUJD\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL,
                "token-less poke refused on the process tier");
    TEST_ASSERT(strstr(m.out, "\"bytes_written\"") == NULL,
                "no device memory was written");
    AuditConfigure(0, NULL);
}

TEST_CASE(mem_wire_roundtrip) {
    /* The positive seam path (NT process tier only): spawnRetain -> peek ->
     * poke -> terminate over ProcessCommand, exercising every HandleMem ok
     * response builder. Byte-for-byte RPM/WPM correctness is pinned in
     * test_mem_ops.c; here we assert the wire shapes. */
    MockTransport m;
    char token[40];
    HANDLE hChild;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long scan;
    unsigned long rwAddr;
    char json[512];

    FeatInit();
    if (!g_features.is_nt) {
        printf("(skip: process tier requires NT) ");
        return;
    }

    ExecConfigure(NULL, 1);     /* unsafe -> catalog-independent spawn */
    AuditConfigure(1, NULL);    /* arm + default (writable) audit sink */
    MemReleaseAll();

    /* spawnRetain mem_target (resolved from the test's working dir). */
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"spawnRetain\",\"id\":\"r1\","
                   "\"argv\":[\"mem_target\"]}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "spawn ok");
    TEST_ASSERT(strstr(m.out, "\"token\":\"m") != NULL, "token returned");
    TEST_ASSERT(strstr(m.out, "\"pid\":") != NULL, "pid returned");
    TEST_ASSERT(extract_json_str(m.out, "token", token, (int)sizeof(token)),
                "token extracted");

    /* Find a committed read/write region in the child to exercise against. */
    hChild = MemTokenHandle(token);
    TEST_ASSERT(hChild != NULL, "token resolves to the retained handle");
    rwAddr = 0;
    scan = 0;
    Sleep(200);   /* let the child's loader settle (see test_mem_ops note) */
    for (;;) {
        if (VirtualQueryEx(hChild, (LPCVOID)scan, &mbi, sizeof(mbi)) !=
            sizeof(mbi)) {
            break;
        }
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE &&
            mbi.RegionSize >= 4096) {
            rwAddr = (unsigned long)mbi.BaseAddress;
            break;
        }
        scan = (unsigned long)mbi.BaseAddress + (unsigned long)mbi.RegionSize;
        if (scan == 0) {
            break;
        }
    }
    TEST_ASSERT(rwAddr != 0, "found a writable region in the child");

    /* peek 8 bytes -> ok response with data_b64 + bytes_read. */
    MockTransportInit(&m, NULL, 0);
    wsprintfA(json, "{\"cmd\":\"peek\",\"id\":\"r2\",\"mem_token\":\"%s\","
              "\"mem_addr\":\"0x%lX\",\"mem_len\":\"8\"}", token, rwAddr);
    ProcessCommand(json, &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "peek ok");
    TEST_ASSERT(strstr(m.out, "\"data_b64\":\"") != NULL, "data_b64 present");
    TEST_ASSERT(strstr(m.out, "\"bytes_read\":8") != NULL, "8 bytes read");

    /* poke 3 bytes ("ABC" = QUJD) -> ok response with bytes_written. */
    MockTransportInit(&m, NULL, 0);
    wsprintfA(json, "{\"cmd\":\"poke\",\"id\":\"r3\",\"mem_token\":\"%s\","
              "\"mem_addr\":\"0x%lX\",\"data\":\"QUJD\"}", token, rwAddr);
    ProcessCommand(json, &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "poke ok");
    TEST_ASSERT(strstr(m.out, "\"bytes_written\":3") != NULL, "3 bytes written");

    /* terminate -> ok response. */
    MockTransportInit(&m, NULL, 0);
    wsprintfA(json, "{\"cmd\":\"terminate\",\"id\":\"r4\",\"mem_token\":\"%s\"}",
              token);
    ProcessCommand(json, &m.t);
    TEST_ASSERT(strstr(m.out, "\"terminated\":true") != NULL, "terminate ok");

    AuditConfigure(0, NULL);    /* restore disarmed state */
    ExecConfigure(NULL, 0);
}

/* ========================================================
 * 5.5 listCommands discovery verb (full JSON -> ProcessCommand
 * -> ok envelope). Obligations (tests/OBLIGATIONS-5.5.md):
 * rule-success.ListCommandsCommand (dispatch_list_commands),
 * rule-success.ListCommandsResult / CatalogListed
 * (list_commands_round_trip), and the existing unknown-command path
 * for a still-unknown verb.
 * ======================================================== */

TEST_CASE(dispatch_list_commands) {
    /* The additive verb reaches its handler via the main loop: formerly
     * "unknown command", now dispatched to an ok listing. */
    MockTransport m;
    Catalog *cat;
    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"listCommands\",\"id\":\"d1\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"id\":\"d1\"") != NULL, "id echoed");
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL,
                "listCommands dispatched (not unknown command)");
    TEST_ASSERT(strstr(m.out, "unknown command") == NULL,
                "no longer an unknown verb");
    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

TEST_CASE(list_commands_round_trip) {
    /* CatalogListingReady -> ok {"status":"ok","commands":[...]} carrying the
     * serialised loaded catalog; a still-unknown verb stays "unknown command". */
    MockTransport m;
    Catalog *cat;
    cat = load_test_catalog();
    TEST_ASSERT(cat != NULL, "test catalog loads");
    ExecConfigure(cat, 0);

    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"listCommands\",\"id\":\"rt1\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"ok\"") != NULL, "ok status");
    TEST_ASSERT(strstr(m.out, "\"commands\":[") != NULL,
                "commands key carries a JSON array");
    /* The loaded catalog (dir is a known builtin) rides back in the listing. */
    TEST_ASSERT(strstr(m.out, "\"name\":\"dir\"") != NULL,
                "loaded entry present in the listing");
    TEST_ASSERT(strstr(m.out, "\"builtin\":true") != NULL,
                "builtin wire field present");

    /* A still-unknown verb is still rejected (the whitelist only grew). */
    MockTransportInit(&m, NULL, 0);
    ProcessCommand("{\"cmd\":\"bogus\",\"id\":\"rt2\"}", &m.t);
    TEST_ASSERT(strstr(m.out, "\"status\":\"error\"") != NULL,
                "bogus is an error");
    TEST_ASSERT(strstr(m.out, "unknown command") != NULL,
                "bogus is still unknown command");

    ExecConfigure(NULL, 0);
    CatalogFree(cat);
}

/* ========================================================
 * 6.2 direct-UART tier gate (SECURITY PIN #1,
 * uart.allium ServingViaUartImpliesWin32s). The dispatch decision is pinned
 * WITHOUT driving a real port (a ring-3 IN #GPs on the CI/NT host); each test
 * restores g_features via FeatInit() so later tests are unaffected.
 * ======================================================== */

TEST_CASE(uart_tier_gate_decision) {
    /* SECURITY PIN #1 (ServingViaUartImpliesWin32s): the gate is exactly the
     * tier - reads g_features, no I/O. */
    FeatInit();
    g_features.is_win32s = 1; g_features.is_nt = 0;
    TEST_ASSERT_INT_EQUAL(1, UartTierWantsDirect(),
                          "win32s => direct route wanted");
    g_features.is_win32s = 0; g_features.is_nt = 1;
    TEST_ASSERT_INT_EQUAL(0, UartTierWantsDirect(),
                          "non-win32s => direct NOT wanted");
    FeatInit();
}

TEST_CASE(uart_dispatch_win32s_selects_direct) {
    /* On the Win32s tier SerialBackendOpen takes the direct-UART branch. Under
     * TEST_BUILD that is a DRY-RUN: it records the route and returns 0 WITHOUT
     * any port I/O (a ring-3 IN would #GP on CI/NT). */
    TransportConfig cfg; Transport out; char err[128]; int rc;
    FeatInit();
    memset(&cfg, 0, sizeof(cfg));
    memset(&out, 0, sizeof(out));
    lstrcpynA(cfg.port, "COM1", (int)sizeof(cfg.port));
    cfg.baudRate = 19200;
    g_features.is_win32s = 1; g_features.is_nt = 0;
    rc = SerialBackendOpen(&cfg, &out, err, (int)sizeof(err));
    TEST_ASSERT_INT_EQUAL(UART_ROUTE_DIRECT_UART, UartLastRouteForTest(),
        "win32s tier selects the direct-UART route");
    TEST_ASSERT_INT_EQUAL(0, rc, "test dry-run returns 0 (no real port I/O)");
    FeatInit();
}

TEST_CASE(uart_dispatch_non_win32s_selects_os_serial) {
    /* Off Win32s the gate lets the OS serial (CreateFileA) path run - proving
     * the direct branch is unreachable off the tier (incl. the /AUTO re-entry). */
    TransportConfig cfg; Transport out; char err[128];
    FeatInit();
    memset(&cfg, 0, sizeof(cfg));
    memset(&out, 0, sizeof(out));
    lstrcpynA(cfg.port, "COM_DOES_NOT_EXIST", (int)sizeof(cfg.port));
    cfg.baudRate = 19200;
    g_features.is_win32s = 0; g_features.is_nt = 1;
    (void)SerialBackendOpen(&cfg, &out, err, (int)sizeof(err));
    TEST_ASSERT_INT_EQUAL(UART_ROUTE_OS_SERIAL, UartLastRouteForTest(),
        "non-win32s tier selects the OS serial route");
    FeatInit();
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
    RUN_TEST(dispatch_copy_ok_envelope);
    RUN_TEST(dispatch_copy_dest_exists_envelope);
    RUN_TEST(dispatch_move_ok_envelope);
    RUN_TEST(dispatch_move_missing_envelope);
    RUN_TEST(dispatch_mkdir_ok_envelope);
    RUN_TEST(dispatch_mkdir_exists_envelope);
    RUN_TEST(dispatch_rmdir_ok_envelope);
    RUN_TEST(dispatch_rmdir_nonempty_envelope);

    printf("\nexec integration:\n");
    FeatInit();
    RUN_TEST(exec_integration_full_response);
    RUN_TEST(exec_catalog_miss_rejected);
    RUN_TEST(exec_unsafe_bypasses_catalog);
    RUN_TEST(exec_builtin_autoroutes_via_shell);
    RUN_TEST(exec_builtin_positional_metachar_neutralised);
    RUN_TEST(exec_output_is_valid_utf8);
    RUN_TEST(exec_busy_carries_detail_then_reaps);
    RUN_TEST(exec_stdin_too_large_rejected);
    RUN_TEST(exec_invalid_stdin_b64_rejected);
    RUN_TEST(ptyexec_capability_absent_error);
    RUN_TEST(ptyexec_output_kind_utf8);

    printf("\nReady message (features.toolchains):\n");
    RUN_TEST(ready_lists_detected_toolchains);

    printf("\nMemory peek/poke wire dispatch:\n");
    RUN_TEST(mem_ready_carries_tier);
    RUN_TEST(encoding_ready_carries_tag);
    RUN_TEST(mem_spawn_retain_uncatalogued_refused);
    RUN_TEST(mem_poke_unarmed_refused);
    RUN_TEST(mem_peek_bad_token_errors);
    RUN_TEST(mem_process_tier_requires_token);
    RUN_TEST(mem_wire_roundtrip);

    printf("\nlistCommands discovery:\n");
    RUN_TEST(dispatch_list_commands);
    RUN_TEST(list_commands_round_trip);

    RUN_TEST(uart_tier_gate_decision);
    RUN_TEST(uart_dispatch_win32s_selects_direct);
    RUN_TEST(uart_dispatch_non_win32s_selects_os_serial);

    print_test_summary();
    return g_tests_failed;
}
