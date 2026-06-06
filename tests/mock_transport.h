/*
 * mock_transport.h - In-memory Transport backend for tests
 *
 * read() delivers scripted input then 0 (orderly close); write()
 * captures bytes into out[] so tests can assert exact response bytes.
 * Set shortWrite > 0 to force partial writes and exercise
 * TransportWriteAll's loop.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H

#include "transport.h"
#include "common.h"

typedef struct {
    Transport t;            /* MUST be first: &m.t aliases the MockTransport */
    const char *in;         /* scripted input bytes */
    int inLen;
    int inPos;
    char out[MCP_MAX_RESPONSE];
    int outLen;
    int shortWrite;         /* if > 0, write() returns at most this many */
    int closed;             /* close() call count */
} MockTransport;

void MockTransportInit(MockTransport *m, const char *in, int inLen);

#endif /* MOCK_TRANSPORT_H */
