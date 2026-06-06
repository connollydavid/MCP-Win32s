/*
 * tcp.h - TCP transport backend for MCP-Win32s (Winsock 1.1)
 *
 * Runtime-loaded via LoadLibraryA("wsock32.dll") + GetProcAddress, so
 * mcp-w32s.exe carries NO static wsock32 import and still loads on bare
 * Win32s without TCP/IP-32. Single-client-sequential server.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef TCP_H
#define TCP_H

#include "transport.h"

/* 1 if wsock32 loads and resolves the Winsock 1.1 entry points. */
int  TcpBackendProbe(void);

/* Open a TCP listener Transport (bind + listen) for cfg->tcpPort. */
int  TcpBackendOpen(const TransportConfig *cfg, Transport *out,
                    char *err, int errSize);

/* WSACleanup at process shutdown (no-op if Winsock never started). */
void TcpBackendCleanup(void);

/* Register the TCP backend in the registry. */
void TcpBackendRegister(void);

/* Host-to-network byte order, implemented by hand (no bswap = i386-safe,
 * and avoids importing htons/htonl from wsock32). */
unsigned short McpHtons(unsigned short x);
unsigned long  McpHtonl(unsigned long x);

#endif /* TCP_H */
