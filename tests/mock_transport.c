/*
 * mock_transport.c - In-memory Transport backend for tests
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "mock_transport.h"

static int mock_read(Transport *t, void *buf, int len)
{
    MockTransport *m;
    int avail;
    int n;

    m = (MockTransport *)t;
    avail = m->inLen - m->inPos;
    if (avail <= 0) {
        return 0;   /* orderly close */
    }
    n = (len < avail) ? len : avail;
    memcpy(buf, m->in + m->inPos, (size_t)n);
    m->inPos += n;
    return n;
}

static int mock_write(Transport *t, const void *buf, int len)
{
    MockTransport *m;
    int n;
    int space;

    m = (MockTransport *)t;
    n = len;
    if (m->shortWrite > 0 && n > m->shortWrite) {
        n = m->shortWrite;
    }
    space = (int)sizeof(m->out) - m->outLen;
    if (n > space) {
        n = space;
    }
    if (n > 0) {
        memcpy(m->out + m->outLen, buf, (size_t)n);
        m->outLen += n;
    }
    return n;
}

static void mock_close(Transport *t)
{
    MockTransport *m;
    m = (MockTransport *)t;
    m->closed++;
}

void MockTransportInit(MockTransport *m, const char *in, int inLen)
{
    memset(m, 0, sizeof(*m));
    m->t.name = "mock";
    m->t.kind = TRANSPORT_MOCK;
    m->t.flags = 0;
    m->t.read = mock_read;
    m->t.write = mock_write;
    m->t.close = mock_close;
    m->t.accept = NULL;
    m->in = in;
    m->inLen = inLen;
    m->inPos = 0;
}
