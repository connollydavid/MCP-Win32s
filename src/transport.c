/*
 * transport.c - Transport registry, selection, and command-line parsing.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "transport.h"

/* ========================================================
 * Command-line parsing (moved here from serial.c - it is
 * transport-level, not serial-level)
 * ======================================================== */

/*
 * find_flag - Find a /FLAG: style argument in a command-line string.
 * Returns pointer to the character after the colon, or NULL if not found.
 * Case-insensitive comparison for the flag name.
 */
static const char *find_flag(const char *cmdLine, const char *flag)
{
    const char *p;
    int flagLen;

    if (cmdLine == NULL || flag == NULL) {
        return NULL;
    }

    flagLen = 0;
    while (flag[flagLen] != '\0') {
        flagLen++;
    }

    p = cmdLine;
    while (*p != '\0') {
        if (*p == '/') {
            const char *start;
            int match;
            int i;

            start = p + 1;
            match = 1;
            for (i = 0; i < flagLen && start[i] != '\0'; i++) {
                char a, b;
                a = start[i];
                b = flag[i];
                if (a >= 'a' && a <= 'z') a = a - ('a' - 'A');
                if (b >= 'a' && b <= 'z') b = b - ('a' - 'A');
                if (a != b) {
                    match = 0;
                    break;
                }
            }
            if (match && i == flagLen && start[i] == ':') {
                return &start[i + 1];
            }
        }
        p++;
    }

    return NULL;
}

/*
 * copy_until_space - Copy chars from src to dst until whitespace or end.
 * Null-terminates dst. Returns number of characters copied.
 */
static int copy_until_space(const char *src, char *dst, int dst_size)
{
    int i;

    i = 0;
    while (src[i] != '\0' && src[i] != ' ' && src[i] != '\t' &&
           i < dst_size - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

/*
 * simple_atoi - Convert decimal string to integer. Stops at first
 * non-digit. Returns 0 for empty/invalid input.
 */
static int simple_atoi(const char *s)
{
    int result;

    result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result;
}

/*
 * find_flag_tok - Like find_flag, but the flag need not carry a value.
 * Returns a pointer to the character immediately after the flag name (':'
 * when a value follows, or a boundary char such as space/end), or NULL if
 * the flag token is absent. Used for /AUTO, whose port is optional.
 */
static const char *find_flag_tok(const char *cmdLine, const char *flag)
{
    const char *p;
    int flagLen;

    if (cmdLine == NULL || flag == NULL) {
        return NULL;
    }

    flagLen = 0;
    while (flag[flagLen] != '\0') {
        flagLen++;
    }

    p = cmdLine;
    while (*p != '\0') {
        if (*p == '/') {
            const char *start;
            int match;
            int i;
            char c;

            start = p + 1;
            match = 1;
            for (i = 0; i < flagLen && start[i] != '\0'; i++) {
                char a, b;
                a = start[i];
                b = flag[i];
                if (a >= 'a' && a <= 'z') a = a - ('a' - 'A');
                if (b >= 'a' && b <= 'z') b = b - ('a' - 'A');
                if (a != b) {
                    match = 0;
                    break;
                }
            }
            if (match && i == flagLen) {
                c = start[i];
                if (c == ':' || c == ' ' || c == '\t' || c == '\0') {
                    return &start[i];
                }
            }
        }
        p++;
    }

    return NULL;
}

int ParseCommandLine(const char *cmdLine, TransportConfig *config)
{
    const char *val;

    if (config == NULL) {
        return 0;
    }

    memset(config, 0, sizeof(TransportConfig));

    /* Default to serial on COM1 */
    config->transport = TRANSPORT_SERIAL;
    copy_until_space(DEFAULT_PORT, config->port, sizeof(config->port));
    config->baudRate = DEFAULT_BAUD_RATE;
    config->autodetect = 0;

    if (cmdLine == NULL || cmdLine[0] == '\0') {
        return 1;
    }

    /* Optional bind-address modifier (TCP only; "" => INADDR_ANY). */
    val = find_flag(cmdLine, "BIND");
    if (val != NULL) {
        copy_until_space(val, config->bindAddr, sizeof(config->bindAddr));
    }

    val = find_flag(cmdLine, "SERIAL");
    if (val != NULL) {
        config->transport = TRANSPORT_SERIAL;
        copy_until_space(val, config->port, sizeof(config->port));
        config->baudRate = DEFAULT_BAUD_RATE;
        return 1;
    }

    /* /AUTO[:port] - try TCP, fall back to serial if Winsock is absent. */
    {
        const char *a;
        a = find_flag_tok(cmdLine, "AUTO");
        if (a != NULL) {
            config->transport = TRANSPORT_TCP;
            config->autodetect = 1;
            if (*a == ':') {
                char portStr[16];
                copy_until_space(a + 1, portStr, sizeof(portStr));
                config->tcpPort = simple_atoi(portStr);
            } else {
                config->tcpPort = DEFAULT_TCP_PORT;
            }
            if (config->tcpPort <= 0 || config->tcpPort > 65535) {
                return 0;
            }
            return 1;
        }
    }

    val = find_flag(cmdLine, "TCP");
    if (val != NULL) {
        char portStr[16];
        config->transport = TRANSPORT_TCP;
        copy_until_space(val, portStr, sizeof(portStr));
        config->tcpPort = simple_atoi(portStr);
        if (config->tcpPort <= 0 || config->tcpPort > 65535) {
            return 0;
        }
        return 1;
    }

    val = find_flag(cmdLine, "PIPE");
    if (val != NULL) {
        config->transport = TRANSPORT_PIPE;
        copy_until_space(val, config->pipeName, sizeof(config->pipeName));
        return 1;
    }

    /* No recognized flag - keep defaults (serial COM1) */
    return 1;
}

/* ========================================================
 * Registry + selection
 * ======================================================== */

#define MAX_BACKENDS 8

static TransportBackend g_backends[MAX_BACKENDS];
static int g_backend_count = 0;

static void err_set(char *err, int errSize, const char *msg)
{
    int i;
    if (err == NULL || errSize <= 0) {
        return;
    }
    for (i = 0; msg[i] != '\0' && i < errSize - 1; i++) {
        err[i] = msg[i];
    }
    err[i] = '\0';
}

static TransportBackend *find_backend(int kind)
{
    int i;
    for (i = 0; i < g_backend_count; i++) {
        if (g_backends[i].kind == kind) {
            return &g_backends[i];
        }
    }
    return NULL;
}

int TransportRegister(const TransportBackend *backend)
{
    if (backend == NULL || g_backend_count >= MAX_BACKENDS) {
        return 0;
    }
    g_backends[g_backend_count] = *backend;
    g_backend_count++;
    return 1;
}

void TransportResetRegistry(void)
{
    g_backend_count = 0;
}

int TransportOpen(const TransportConfig *cfg, Transport *out,
                  char *err, int errSize)
{
    TransportBackend *b;
    TransportBackend *s;

    if (cfg == NULL || out == NULL) {
        err_set(err, errSize, "null config");
        return 0;
    }

    b = find_backend(cfg->transport);
    if (b != NULL && (b->probe == NULL || b->probe())) {
        if (b->open(cfg, out, err, errSize)) {
            return 1;
        }
    }

    /* Fallback to serial when auto-detecting. */
    if (cfg->autodetect) {
        s = find_backend(TRANSPORT_SERIAL);
        if (s != NULL && (s->probe == NULL || s->probe())) {
            if (s->open(cfg, out, err, errSize)) {
                return 1;
            }
        }
    }

    if (b == NULL) {
        err_set(err, errSize, "no backend for requested transport");
    } else if (b->probe != NULL && !b->probe()) {
        err_set(err, errSize, "requested transport not available on this host");
    }
    return 0;
}

int TransportWriteAll(Transport *t, const void *buf, int len)
{
    const char *p;
    int total;
    int n;

    if (t == NULL || t->write == NULL) {
        return -1;
    }
    p = (const char *)buf;
    total = 0;
    while (total < len) {
        n = t->write(t, p + total, len - total);
        if (n <= 0) {
            return -1;
        }
        total += n;
    }
    return total;
}

const char *TransportName(const Transport *t)
{
    if (t == NULL || t->name == NULL) {
        return "";
    }
    return t->name;
}
