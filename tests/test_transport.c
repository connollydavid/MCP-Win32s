/*
 * test_transport.c - Unit tests for the transport abstraction
 *
 * Registry lookup, backend selection + serial fallback, TransportWriteAll
 * short-write looping, and the mock backend's read/write/close. No real
 * ports or sockets are opened.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "transport.h"
#include "mock_transport.h"

/* ---- stub backends for selection tests ---- */

static int g_tcp_probe_result = 1;

static int serial_stub_open(const TransportConfig *cfg, Transport *out,
                            char *err, int errSize)
{
    (void)cfg; (void)err; (void)errSize;
    memset(out, 0, sizeof(*out));
    out->name = "serial";
    out->kind = TRANSPORT_SERIAL;
    return 1;
}

static int tcp_stub_open(const TransportConfig *cfg, Transport *out,
                         char *err, int errSize)
{
    (void)cfg; (void)err; (void)errSize;
    memset(out, 0, sizeof(*out));
    out->name = "tcp";
    out->kind = TRANSPORT_TCP;
    return 1;
}

static int tcp_probe(void)
{
    return g_tcp_probe_result;
}

static void register_serial(void)
{
    TransportBackend b;
    b.kind = TRANSPORT_SERIAL;
    b.name = "serial";
    b.probe = NULL;
    b.open = serial_stub_open;
    TransportRegister(&b);
}

static void register_tcp(int available)
{
    TransportBackend b;
    g_tcp_probe_result = available;
    b.kind = TRANSPORT_TCP;
    b.name = "tcp";
    b.probe = tcp_probe;
    b.open = tcp_stub_open;
    TransportRegister(&b);
}

/* ========================================================
 * Selection / registry
 * ======================================================== */

TEST_CASE(open_selects_serial) {
    TransportConfig cfg;
    Transport t;
    char err[64];
    int r;
    TransportResetRegistry();
    register_serial();
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_SERIAL;
    r = TransportOpen(&cfg, &t, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, r, "serial opens");
    TEST_ASSERT_STR_EQUAL("serial", t.name, "serial backend selected");
}

TEST_CASE(open_selects_tcp) {
    TransportConfig cfg;
    Transport t;
    char err[64];
    int r;
    TransportResetRegistry();
    register_serial();
    register_tcp(1);
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    r = TransportOpen(&cfg, &t, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, r, "tcp opens");
    TEST_ASSERT_STR_EQUAL("tcp", t.name, "tcp backend selected");
}

TEST_CASE(open_unknown_no_autodetect_fails) {
    TransportConfig cfg;
    Transport t;
    char err[64];
    int r;
    TransportResetRegistry();
    register_serial();
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;   /* not registered */
    cfg.autodetect = 0;
    r = TransportOpen(&cfg, &t, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, r, "explicit unavailable backend fails");
}

TEST_CASE(open_autodetect_falls_back_to_serial) {
    TransportConfig cfg;
    Transport t;
    char err[64];
    int r;
    TransportResetRegistry();
    register_serial();
    register_tcp(0);                 /* tcp present but probe fails */
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    cfg.autodetect = 1;
    r = TransportOpen(&cfg, &t, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, r, "falls back");
    TEST_ASSERT_STR_EQUAL("serial", t.name, "fallback is serial");
}

TEST_CASE(open_unavailable_probe_reports_error) {
    TransportConfig cfg;
    Transport t;
    char err[64];
    int r;
    TransportResetRegistry();
    register_tcp(0);                 /* probe fails, no autodetect */
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    cfg.autodetect = 0;
    err[0] = '\0';
    r = TransportOpen(&cfg, &t, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, r, "probe-fail without autodetect fails");
    TEST_ASSERT(err[0] != '\0', "error message set");
}

TEST_CASE(open_null_config_fails) {
    Transport t;
    char err[64];
    int r;
    TransportResetRegistry();
    r = TransportOpen(NULL, &t, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(0, r, "NULL config fails");
}

/* ========================================================
 * TransportWriteAll over the mock backend
 * ======================================================== */

TEST_CASE(writeall_full) {
    MockTransport m;
    int r;
    MockTransportInit(&m, NULL, 0);
    r = TransportWriteAll(&m.t, "0123456789", 10);
    TEST_ASSERT_INT_EQUAL(10, r, "all bytes written");
    TEST_ASSERT_INT_EQUAL(10, m.outLen, "captured length");
    TEST_ASSERT(memcmp(m.out, "0123456789", 10) == 0, "captured bytes");
}

TEST_CASE(writeall_short_writes_loop) {
    MockTransport m;
    int r;
    MockTransportInit(&m, NULL, 0);
    m.shortWrite = 1;   /* one byte per write() call */
    r = TransportWriteAll(&m.t, "abcdef", 6);
    TEST_ASSERT_INT_EQUAL(6, r, "loops over short writes");
    TEST_ASSERT_INT_EQUAL(6, m.outLen, "all captured");
    TEST_ASSERT(memcmp(m.out, "abcdef", 6) == 0, "bytes intact");
}

TEST_CASE(writeall_null_transport) {
    int r;
    r = TransportWriteAll(NULL, "x", 1);
    TEST_ASSERT_INT_EQUAL(-1, r, "NULL transport returns error");
}

/* ========================================================
 * Mock read / close / vtable shape
 * ======================================================== */

TEST_CASE(mock_read_then_close) {
    MockTransport m;
    char b[8];
    int n;
    MockTransportInit(&m, "abc", 3);
    n = m.t.read(&m.t, b, (int)sizeof(b));
    TEST_ASSERT_INT_EQUAL(3, n, "reads scripted bytes");
    TEST_ASSERT(memcmp(b, "abc", 3) == 0, "content matches");
    n = m.t.read(&m.t, b, (int)sizeof(b));
    TEST_ASSERT_INT_EQUAL(0, n, "then orderly close");
}

TEST_CASE(mock_read_partial) {
    MockTransport m;
    char b[2];
    int n;
    MockTransportInit(&m, "abcd", 4);
    n = m.t.read(&m.t, b, 2);
    TEST_ASSERT_INT_EQUAL(2, n, "reads up to buffer size");
    n = m.t.read(&m.t, b, 2);
    TEST_ASSERT_INT_EQUAL(2, n, "reads remainder");
}

TEST_CASE(mock_accept_is_null) {
    MockTransport m;
    MockTransportInit(&m, NULL, 0);
    TEST_ASSERT(m.t.accept == NULL, "point-to-point has no accept");
}

TEST_CASE(mock_double_close_safe) {
    MockTransport m;
    MockTransportInit(&m, NULL, 0);
    m.t.close(&m.t);
    m.t.close(&m.t);
    TEST_ASSERT_INT_EQUAL(2, m.closed, "close is idempotent and counted");
}

TEST_CASE(message_flag_roundtrips) {
    MockTransport m;
    MockTransportInit(&m, NULL, 0);
    m.t.flags = TRANSPORT_FLAG_MESSAGE;
    TEST_ASSERT(m.t.flags & TRANSPORT_FLAG_MESSAGE, "message flag set");
}

TEST_CASE(name_null_safe) {
    TEST_ASSERT_STR_EQUAL("", TransportName(NULL), "NULL name -> empty");
}

int main(void)
{
    printf("Transport Tests\n");
    printf("========================================\n");

    printf("\nSelection:\n");
    RUN_TEST(open_selects_serial);
    RUN_TEST(open_selects_tcp);
    RUN_TEST(open_unknown_no_autodetect_fails);
    RUN_TEST(open_autodetect_falls_back_to_serial);
    RUN_TEST(open_unavailable_probe_reports_error);
    RUN_TEST(open_null_config_fails);

    printf("\nTransportWriteAll:\n");
    RUN_TEST(writeall_full);
    RUN_TEST(writeall_short_writes_loop);
    RUN_TEST(writeall_null_transport);

    printf("\nMock backend:\n");
    RUN_TEST(mock_read_then_close);
    RUN_TEST(mock_read_partial);
    RUN_TEST(mock_accept_is_null);
    RUN_TEST(mock_double_close_safe);
    RUN_TEST(message_flag_roundtrips);
    RUN_TEST(name_null_safe);

    print_test_summary();
    return g_tests_failed;
}
