/*
 * transport.h - Backend-agnostic transport abstraction for MCP-Win32s
 *
 * The protocol core speaks to a Transport vtable (read/write/close, and
 * an optional accept for server backends) and never touches a raw HANDLE
 * or SOCKET directly. Backends (serial, tcp, mock, future UDP/QUIC/RDMA)
 * register in a small registry; TransportOpen selects one, with optional
 * fallback to serial. This is the seam that makes the network a first-
 * class peer of the serial port.
 *
 * Newline-JSON framing lives above the transport. A message-oriented
 * backend sets TRANSPORT_FLAG_MESSAGE to bypass framing.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <windows.h>

/* Transport kinds */
#define TRANSPORT_NONE   0
#define TRANSPORT_SERIAL 1
#define TRANSPORT_TCP    2
#define TRANSPORT_PIPE   3    /* reserved - Phase 5+ */
#define TRANSPORT_MOCK   99   /* test-only */

/* Transport flags */
#define TRANSPORT_FLAG_MESSAGE 0x01  /* one message = one command; bypass framing */

/* Serial defaults (transport-level config) */
#define DEFAULT_BAUD_RATE CBR_115200
#define DEFAULT_PORT      "COM1"

/*
 * TransportConfig - parsed command-line transport selection.
 */
typedef struct {
    int   transport;        /* TRANSPORT_SERIAL | TRANSPORT_TCP | ... */
    char  port[32];         /* "COM1" ... (serial) */
    DWORD baudRate;         /* serial */
    int   tcpPort;          /* TCP listen port */
    char  pipeName[260];    /* reserved */
    int   autodetect;       /* 1 = try requested, fall back to serial */
} TransportConfig;

/*
 * Transport - a live byte-pipe with a backend vtable.
 *
 * read/write return: >0 bytes moved, 0 = orderly peer close, <0 = error.
 * accept is NULL for point-to-point backends (serial); for listeners
 * (tcp) it blocks for a client and returns a connection Transport.
 */
typedef struct Transport Transport;
struct Transport {
    const char *name;       /* "serial" | "tcp" | "mock" - surfaced at ready */
    int   kind;
    int   flags;
    int  (*read)(Transport *t, void *buf, int len);
    int  (*write)(Transport *t, const void *buf, int len);
    void (*close)(Transport *t);
    Transport *(*accept)(Transport *t);
    union { HANDLE handle; unsigned int sock; void *ptr; } io;
};

/*
 * TransportBackend - a registry entry. probe returns 1 if usable on this
 * host (NULL = always available). open fills a Transport for cfg.
 */
typedef struct {
    int   kind;
    const char *name;
    int  (*probe)(void);
    int  (*open)(const TransportConfig *cfg, Transport *out,
                 char *err, int errSize);
} TransportBackend;

/* Parse "/SERIAL:COMx", "/TCP:port", "/PIPE:..." into cfg. */
int ParseCommandLine(const char *cmdLine, TransportConfig *cfg);

/* Registry. TransportRegister copies the backend by value. */
int  TransportRegister(const TransportBackend *backend);
void TransportResetRegistry(void);   /* test helper */

/* Select + open a backend (with serial fallback when autodetect). */
int  TransportOpen(const TransportConfig *cfg, Transport *out,
                   char *err, int errSize);

/* Write all bytes, looping over short writes. Returns len or <0. */
int  TransportWriteAll(Transport *t, const void *buf, int len);

/* Backend name, or "" if unavailable. */
const char *TransportName(const Transport *t);

#endif /* TRANSPORT_H */
