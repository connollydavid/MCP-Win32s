/*
 * test_tcp.c - Tests for the TCP backend against REAL Winsock 1.1.
 *
 * On the WSL2+Windows dev host these run natively through real wsock32,
 * so the loopback round-trip genuinely exercises Winsock - it is not
 * skipped. The backend-under-test resolves Winsock at runtime; the test's
 * own client side links wsock32 directly.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <winsock.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "transport.h"
#include "tcp.h"

#define TEST_PORT 8123

/* ========================================================
 * Byte order (manual, no bswap)
 * ======================================================== */

TEST_CASE(htons_swaps) {
    TEST_ASSERT_INT_EQUAL(0x3412, (int)McpHtons(0x1234), "htons swaps bytes");
    TEST_ASSERT_INT_EQUAL(0x00FF, (int)McpHtons(0xFF00), "htons high->low");
}

TEST_CASE(htonl_swaps) {
    TEST_ASSERT(McpHtonl(0x01020304UL) == 0x04030201UL, "htonl swaps 4 bytes");
}

/* ========================================================
 * Probe + listener
 * ======================================================== */

TEST_CASE(probe_succeeds) {
    /* Real Winsock present natively on Windows; must not be skipped. */
    TEST_ASSERT_INT_EQUAL(1, TcpBackendProbe(), "wsock32 resolves");
}

TEST_CASE(listener_binds) {
    TransportConfig cfg;
    Transport listener;
    char err[64];
    int r;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    cfg.tcpPort = TEST_PORT;
    r = TcpBackendOpen(&cfg, &listener, err, sizeof(err));
    TEST_ASSERT_INT_EQUAL(1, r, "listener binds + listens");
    TEST_ASSERT(listener.accept != NULL, "listener has accept");
    listener.close(&listener);
}

/* ========================================================
 * Full loopback round-trip against real Winsock
 * ======================================================== */

TEST_CASE(roundtrip_echo) {
    TransportConfig cfg;
    Transport listener;
    Transport *conn;
    char err[64];
    SOCKET client;
    struct sockaddr_in addr;
    char rbuf[16];
    char cbuf[16];
    int n;

    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    cfg.tcpPort = TEST_PORT + 1;
    TEST_ASSERT_INT_EQUAL(1, TcpBackendOpen(&cfg, &listener, err, sizeof(err)),
                          "open listener");

    /* Client connects into the backlog (completes immediately on loopback). */
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(client != INVALID_SOCKET, "client socket");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = McpHtons(TEST_PORT + 1);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    TEST_ASSERT(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0,
                "client connects");

    /* Server accepts the queued client. */
    conn = listener.accept(&listener);
    TEST_ASSERT(conn != NULL, "accept returns a connection");

    /* Client -> server */
    TEST_ASSERT(send(client, "hi\n", 3, 0) == 3, "client sends");
    n = conn->read(conn, rbuf, (int)sizeof(rbuf));
    TEST_ASSERT_INT_EQUAL(3, n, "server receives 3 bytes");
    TEST_ASSERT(memcmp(rbuf, "hi\n", 3) == 0, "payload intact");

    /* Server -> client */
    TEST_ASSERT_INT_EQUAL(2, conn->write(conn, "ok", 2), "server replies");
    n = recv(client, cbuf, (int)sizeof(cbuf), 0);
    TEST_ASSERT_INT_EQUAL(2, n, "client receives reply");
    TEST_ASSERT(memcmp(cbuf, "ok", 2) == 0, "reply intact");

    /* Orderly close: client goes away, server read returns 0. */
    closesocket(client);
    n = conn->read(conn, rbuf, (int)sizeof(rbuf));
    TEST_ASSERT_INT_EQUAL(0, n, "server sees orderly close");

    conn->close(conn);
    listener.close(&listener);
}

TEST_CASE(sequential_second_client) {
    TransportConfig cfg;
    Transport listener;
    Transport *conn;
    char err[64];
    SOCKET c1;
    SOCKET c2;
    struct sockaddr_in addr;

    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    cfg.tcpPort = TEST_PORT + 2;
    TEST_ASSERT_INT_EQUAL(1, TcpBackendOpen(&cfg, &listener, err, sizeof(err)),
                          "open listener");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = McpHtons(TEST_PORT + 2);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* First client served then disconnected. */
    c1 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(connect(c1, (struct sockaddr *)&addr, sizeof(addr)) == 0,
                "client 1 connects");
    conn = listener.accept(&listener);
    TEST_ASSERT(conn != NULL, "accept client 1");
    closesocket(c1);
    conn->close(conn);

    /* Listener accepts the NEXT client (single-client-sequential). */
    c2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(connect(c2, (struct sockaddr *)&addr, sizeof(addr)) == 0,
                "client 2 connects");
    conn = listener.accept(&listener);
    TEST_ASSERT(conn != NULL, "accept client 2 sequentially");
    closesocket(c2);
    conn->close(conn);

    listener.close(&listener);
}

TEST_CASE(accept_enables_keepalive) {
    TransportConfig cfg;
    Transport listener;
    Transport *conn;
    char err[64];
    SOCKET client;
    struct sockaddr_in addr;
    int ka;
    int kalen;

    memset(&cfg, 0, sizeof(cfg));
    cfg.transport = TRANSPORT_TCP;
    cfg.tcpPort = TEST_PORT + 3;
    TEST_ASSERT_INT_EQUAL(1, TcpBackendOpen(&cfg, &listener, err, sizeof(err)),
                          "open listener");

    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = McpHtons(TEST_PORT + 3);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    TEST_ASSERT(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0,
                "client connects");

    conn = listener.accept(&listener);
    TEST_ASSERT(conn != NULL, "accept returns a connection");

    /* The accepted connection must have keep-alive enabled so a peer that
     * vanishes without FIN is eventually detected. */
    ka = 0;
    kalen = (int)sizeof(ka);
    TEST_ASSERT(getsockopt((SOCKET)conn->io.sock, SOL_SOCKET, SO_KEEPALIVE,
                           (char *)&ka, &kalen) == 0, "getsockopt SO_KEEPALIVE ok");
    TEST_ASSERT(ka != 0, "keep-alive enabled on accepted socket");

    closesocket(client);
    conn->close(conn);
    listener.close(&listener);
}

int main(void)
{
    WSADATA wsa;

    printf("TCP Backend Tests (real Winsock)\n");
    printf("========================================\n");

    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        printf("  FATAL: WSAStartup failed\n");
        return 1;
    }

    printf("\nByte order:\n");
    RUN_TEST(htons_swaps);
    RUN_TEST(htonl_swaps);

    printf("\nProbe + listener:\n");
    RUN_TEST(probe_succeeds);
    RUN_TEST(listener_binds);

    printf("\nLoopback round-trip:\n");
    RUN_TEST(roundtrip_echo);
    RUN_TEST(sequential_second_client);
    RUN_TEST(accept_enables_keepalive);

    TcpBackendCleanup();
    WSACleanup();

    print_test_summary();
    return g_tests_failed;
}
