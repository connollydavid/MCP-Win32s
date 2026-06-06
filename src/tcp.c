/*
 * tcp.c - TCP transport backend for MCP-Win32s (Winsock 1.1)
 *
 * Every Winsock entry point is resolved at runtime from wsock32.dll, so
 * the import table stays clean and the binary still loads on bare Win32s.
 * The server is single-client-sequential: bind + listen(1), accept one
 * client, serve it, then accept the next.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <winsock.h>
#include <windows.h>
#include <string.h>
#include "tcp.h"

/* Winsock 1.1 function-pointer table (resolved via GetProcAddress). */
typedef int    (WINAPI *fn_wsastartup)(WORD, LPWSADATA);
typedef int    (WINAPI *fn_wsacleanup)(void);
typedef SOCKET (WINAPI *fn_socket)(int, int, int);
typedef int    (WINAPI *fn_bind)(SOCKET, const struct sockaddr *, int);
typedef int    (WINAPI *fn_listen)(SOCKET, int);
typedef SOCKET (WINAPI *fn_accept)(SOCKET, struct sockaddr *, int *);
typedef int    (WINAPI *fn_recv)(SOCKET, char *, int, int);
typedef int    (WINAPI *fn_send)(SOCKET, const char *, int, int);
typedef int    (WINAPI *fn_closesocket)(SOCKET);
typedef int    (WINAPI *fn_setsockopt)(SOCKET, int, int, const char *, int);
typedef int    (WINAPI *fn_wsagle)(void);

static struct {
    fn_wsastartup   startup;
    fn_wsacleanup   cleanup;
    fn_socket       socket;
    fn_bind         bind;
    fn_listen       listen;
    fn_accept       accept;
    fn_recv         recv;
    fn_send         send;
    fn_closesocket  closesocket;
    fn_setsockopt   setsockopt;
    fn_wsagle       lasterror;
} g_ws;

static int g_loaded = 0;    /* fn table resolved */
static int g_started = 0;   /* WSAStartup succeeded */

/* Single-client-sequential: one connection object suffices. */
static Transport g_conn;

unsigned short McpHtons(unsigned short x)
{
    return (unsigned short)(((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8));
}

unsigned long McpHtonl(unsigned long x)
{
    return ((x & 0x000000FFUL) << 24) |
           ((x & 0x0000FF00UL) << 8)  |
           ((x & 0x00FF0000UL) >> 8)  |
           ((x & 0xFF000000UL) >> 24);
}

int TcpBackendProbe(void)
{
    HMODULE h;

    if (g_loaded) {
        return 1;
    }

    h = LoadLibraryA("wsock32.dll");
    if (h == NULL) {
        return 0;
    }

    g_ws.startup     = (fn_wsastartup)  GetProcAddress(h, "WSAStartup");
    g_ws.cleanup     = (fn_wsacleanup)  GetProcAddress(h, "WSACleanup");
    g_ws.socket      = (fn_socket)      GetProcAddress(h, "socket");
    g_ws.bind        = (fn_bind)        GetProcAddress(h, "bind");
    g_ws.listen      = (fn_listen)      GetProcAddress(h, "listen");
    g_ws.accept      = (fn_accept)      GetProcAddress(h, "accept");
    g_ws.recv        = (fn_recv)        GetProcAddress(h, "recv");
    g_ws.send        = (fn_send)        GetProcAddress(h, "send");
    g_ws.closesocket = (fn_closesocket) GetProcAddress(h, "closesocket");
    g_ws.setsockopt  = (fn_setsockopt)  GetProcAddress(h, "setsockopt");
    g_ws.lasterror   = (fn_wsagle)      GetProcAddress(h, "WSAGetLastError");

    if (g_ws.startup == NULL || g_ws.cleanup == NULL ||
        g_ws.socket == NULL || g_ws.bind == NULL ||
        g_ws.listen == NULL || g_ws.accept == NULL ||
        g_ws.recv == NULL || g_ws.send == NULL ||
        g_ws.closesocket == NULL || g_ws.setsockopt == NULL ||
        g_ws.lasterror == NULL) {
        return 0;
    }

    g_loaded = 1;
    return 1;
}

static int tcp_recv(Transport *t, void *buf, int len)
{
    int n;
    n = g_ws.recv(t->io.sock, (char *)buf, len, 0);
    if (n == SOCKET_ERROR) {
        return -1;
    }
    return n;   /* 0 = orderly close */
}

static int tcp_send(Transport *t, const void *buf, int len)
{
    int n;
    n = g_ws.send(t->io.sock, (const char *)buf, len, 0);
    if (n == SOCKET_ERROR) {
        return -1;
    }
    return n;
}

static void tcp_sock_close(Transport *t)
{
    if (t->io.sock != (unsigned int)INVALID_SOCKET) {
        g_ws.closesocket(t->io.sock);
        t->io.sock = (unsigned int)INVALID_SOCKET;
    }
}

static Transport *tcp_accept(Transport *listener)
{
    SOCKET c;
    int on;

    c = g_ws.accept(listener->io.sock, NULL, NULL);
    if (c == INVALID_SOCKET) {
        return NULL;
    }

    /*
     * Enable TCP keep-alive so a peer that vanishes WITHOUT sending FIN
     * (crash, pulled cable, NAT idle-timeout - a half-open connection) is
     * eventually detected: recv then fails instead of blocking forever.
     * This is the TCP counterpart to serial having no idle-close. Best
     * effort - if it fails the connection still works, just without
     * dead-peer detection, so the result is intentionally not checked.
     */
    on = 1;
    g_ws.setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, (const char *)&on, sizeof(on));

    g_conn.name = "tcp";
    g_conn.kind = TRANSPORT_TCP;
    g_conn.flags = 0;
    g_conn.read = tcp_recv;
    g_conn.write = tcp_send;
    g_conn.close = tcp_sock_close;
    g_conn.accept = NULL;       /* the connection is not a listener */
    g_conn.io.sock = (unsigned int)c;
    return &g_conn;
}

int TcpBackendOpen(const TransportConfig *cfg, Transport *out,
                   char *err, int errSize)
{
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in addr;

    if (!TcpBackendProbe()) {
        if (err != NULL && errSize > 0) {
            lstrcpynA(err, "winsock (wsock32.dll) not available", errSize);
        }
        return 0;
    }

    if (!g_started) {
        if (g_ws.startup(MAKEWORD(1, 1), &wsa) != 0) {
            if (err != NULL && errSize > 0) {
                lstrcpynA(err, "WSAStartup failed", errSize);
            }
            return 0;
        }
        if (LOBYTE(wsa.wVersion) != 1 || HIBYTE(wsa.wVersion) != 1) {
            g_ws.cleanup();
            if (err != NULL && errSize > 0) {
                lstrcpynA(err, "winsock 1.1 not supported", errSize);
            }
            return 0;
        }
        g_started = 1;
    }

    s = g_ws.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (err != NULL && errSize > 0) {
            lstrcpynA(err, "socket() failed", errSize);
        }
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = McpHtons((unsigned short)cfg->tcpPort);
    addr.sin_addr.s_addr = 0;   /* INADDR_ANY (0, byte-order-agnostic) */

    if (g_ws.bind(s, (const struct sockaddr *)&addr, sizeof(addr)) ==
        SOCKET_ERROR) {
        g_ws.closesocket(s);
        if (err != NULL && errSize > 0) {
            lstrcpynA(err, "bind() failed", errSize);
        }
        return 0;
    }

    if (g_ws.listen(s, 1) == SOCKET_ERROR) {
        g_ws.closesocket(s);
        if (err != NULL && errSize > 0) {
            lstrcpynA(err, "listen() failed", errSize);
        }
        return 0;
    }

    out->name = "tcp";
    out->kind = TRANSPORT_TCP;
    out->flags = 0;
    out->read = tcp_recv;       /* unused on the listener itself */
    out->write = tcp_send;
    out->close = tcp_sock_close;
    out->accept = tcp_accept;   /* listener: blocks for a client */
    out->io.sock = (unsigned int)s;
    return 1;
}

void TcpBackendCleanup(void)
{
    if (g_started) {
        g_ws.cleanup();
        g_started = 0;
    }
}

void TcpBackendRegister(void)
{
    TransportBackend b;
    b.kind = TRANSPORT_TCP;
    b.name = "tcp";
    b.probe = TcpBackendProbe;
    b.open = TcpBackendOpen;
    TransportRegister(&b);
}
