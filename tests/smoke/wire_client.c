/*
 * wire_client.c - Wire-contract smoke client for MCP-Win32s (Phase 4.0b)
 *
 * A standalone test tool that proves the server's side of the client wire
 * contract specified in specs/wire-contract.allium. It is a TEST binary,
 * compiled for the same MinGW i386/C89 target as the server (run natively
 * via WSL interop locally, Wine in CI). Unlike the server it MAY import
 * Winsock 1.1 statically (-lwsock32); the merge gate only forbids static
 * wsock32 in mcp-w32s.exe itself.
 *
 * Usage: wire_client <host> <port> [--ready-only]
 *
 * Ready handshake: connect, read the FIRST line, validate the ready shape
 *   per contract ReadyHandshake (status:"ready", codepage Integer, version
 *   non-empty String, transport non-empty, features object with the eight
 *   documented boolean keys). Extra keys are tolerated (a missing catalog
 *   adds a "warning" key). A malformed/non-ready first line fails the
 *   session (rule ReadyMalformed) -> nonzero exit.
 *
 * Request/response: round trips correlated by id (rule
 *   ResponseCorrelated; invariant ResponsesMatchTheirRequest), sent
 *   strictly sequentially (invariant OneLineOneMessage):
 *     echo  -> status ok, data=="ping", id=="w1"
 *     exec  -> status ok, exit_code 0, stdout_b64 present, id=="w2"
 *     error -> status error, id=="w3"
 *
 * Property-based: in-process tests (prop.h) against the tolerant
 *   response parser only (no network):
 *     a. correlation ids round-trip (rule-success.ResponseCorrelated PBT)
 *     b. unknown response keys are ignored (RequestResponse @guidance)
 *     c. malformed responses never crash the parser (RequestResponse
 *        @guidance / contract-guidance client robustness)
 *
 * The tolerant response parser (wire_parse_response below) is the client's
 * OWN parser, deliberately separate from src/json_parser.c's
 * ParseJsonCommand (which parses the SERVER's command shape). It scans
 * key/value pairs, ignores unknown keys, and never crashes on malformed
 * input.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <winsock.h>     /* Winsock 1.1 (wsock32) - test binary only */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROP_IMPLEMENTATION
#include "../prop.h"

#define WC_LINE_MAX   8192
#define WC_VAL_MAX    4096

/* ------------------------------------------------------------------ */
/* Tolerant response parser (client-side, separate from json_parser.c) */
/* ------------------------------------------------------------------ */

/*
 * Extracted view of a response line. Only the keys the smoke harness
 * cares about are surfaced; all other keys are scanned past and ignored
 * (RequestResponse @guidance: clients must ignore unknown response keys).
 */
typedef struct {
    int  ok;                 /* 1 = a JSON object was parsed, 0 = malformed */
    char id[WC_VAL_MAX];     /* "id" string value ("" if absent)            */
    char status[64];         /* "status" string value ("" if absent)        */
    char data[WC_VAL_MAX];   /* "data" string value (echo)                  */
    int  has_exit_code;      /* "exit_code" integer present                 */
    int  exit_code;
    int  has_stdout_b64;     /* "stdout_b64" key present                    */
    char stdout_b64[WC_VAL_MAX];
} WireResponse;

/* skip ASCII whitespace */
static const char *wc_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/*
 * wc_parse_string - parse a JSON string starting at the opening quote.
 * Writes the unescaped value into out (bounded by outSize, always NUL-
 * terminated). Returns pointer past the closing quote, or NULL on a
 * malformed (unterminated) string. Never reads past the terminating NUL.
 */
static const char *wc_parse_string(const char *p, char *out, int outSize)
{
    int n;

    if (*p != '"') {
        return NULL;
    }
    p++;
    n = 0;
    while (*p != '\0' && *p != '"') {
        char c;
        if (*p == '\\') {
            p++;
            if (*p == '\0') {
                return NULL;   /* dangling escape */
            }
            switch (*p) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case '/':  c = '/';  break;
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            case 'u':
                /* tolerate \uXXXX: emit '?', skip up to 4 hex digits */
                c = '?';
                {
                    int k;
                    for (k = 0; k < 4; k++) {
                        char h = p[1];
                        if ((h >= '0' && h <= '9') ||
                            (h >= 'a' && h <= 'f') ||
                            (h >= 'A' && h <= 'F')) {
                            p++;
                        } else {
                            break;
                        }
                    }
                }
                break;
            default:   c = *p;   break;
            }
        } else {
            c = *p;
        }
        if (n < outSize - 1) {
            out[n++] = c;
        }
        p++;
    }
    if (*p != '"') {
        return NULL;           /* unterminated string */
    }
    out[n] = '\0';
    return p + 1;              /* past closing quote */
}

/*
 * wc_skip_value - scan past a JSON value (string, number, bool, null,
 * object or array) without interpreting it. Used to ignore unknown keys.
 * Returns pointer past the value, or NULL on malformed input. Never reads
 * past the terminating NUL.
 */
static const char *wc_skip_value(const char *p)
{
    char scratch[WC_VAL_MAX];

    p = wc_skip_ws(p);
    if (*p == '"') {
        return wc_parse_string(p, scratch, (int)sizeof(scratch));
    }
    if (*p == '{' || *p == '[') {
        char open  = *p;
        char close = (open == '{') ? '}' : ']';
        int  depth = 0;
        while (*p != '\0') {
            if (*p == '"') {
                /* skip nested strings so braces inside them don't count */
                p = wc_parse_string(p, scratch, (int)sizeof(scratch));
                if (p == NULL) {
                    return NULL;
                }
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;           /* unbalanced */
    }
    /* number / true / false / null: consume until delimiter */
    if (*p == '\0' || *p == ',' || *p == '}' || *p == ']') {
        return NULL;           /* missing value */
    }
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/* parse a non-negative or negative integer; returns 1 on success */
static int wc_parse_int(const char *p, int *out, const char **endp)
{
    int sign = 1;
    int val  = 0;
    int any  = 0;

    p = wc_skip_ws(p);
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        any = 1;
        p++;
    }
    if (!any) {
        return 0;
    }
    *out  = val * sign;
    *endp = p;
    return 1;
}

/*
 * wire_parse_response - the tolerant response parser. Scans one response
 * line. Extracts id/status/data/exit_code/stdout_b64; ignores every other
 * key. Sets out->ok=0 (never crashes) on any malformed input. This is the
 * single function the PBT properties hammer.
 */
static void wire_parse_response(const char *line, WireResponse *out)
{
    const char *p;

    memset(out, 0, sizeof(*out));
    if (line == NULL) {
        return;
    }

    p = wc_skip_ws(line);
    if (*p != '{') {
        return;                /* not a JSON object */
    }
    p++;
    p = wc_skip_ws(p);
    if (*p == '}') {
        out->ok = 1;           /* empty object is well-formed */
        return;
    }

    for (;;) {
        char key[128];
        const char *next;

        p = wc_skip_ws(p);
        next = wc_parse_string(p, key, (int)sizeof(key));
        if (next == NULL) {
            return;            /* malformed key */
        }
        p = wc_skip_ws(next);
        if (*p != ':') {
            return;            /* missing colon */
        }
        p = wc_skip_ws(p + 1);

        if (strcmp(key, "id") == 0) {
            next = wc_parse_string(p, out->id, (int)sizeof(out->id));
            if (next == NULL) {
                return;
            }
            p = next;
        } else if (strcmp(key, "status") == 0) {
            next = wc_parse_string(p, out->status, (int)sizeof(out->status));
            if (next == NULL) {
                return;
            }
            p = next;
        } else if (strcmp(key, "data") == 0) {
            next = wc_parse_string(p, out->data, (int)sizeof(out->data));
            if (next == NULL) {
                return;
            }
            p = next;
        } else if (strcmp(key, "stdout_b64") == 0) {
            next = wc_parse_string(p, out->stdout_b64,
                                   (int)sizeof(out->stdout_b64));
            if (next == NULL) {
                return;
            }
            out->has_stdout_b64 = 1;
            p = next;
        } else if (strcmp(key, "exit_code") == 0) {
            int iv;
            if (!wc_parse_int(p, &iv, &next)) {
                return;
            }
            out->exit_code     = iv;
            out->has_exit_code = 1;
            p = next;
        } else {
            /* unknown key: skip its value, keep going */
            next = wc_skip_value(p);
            if (next == NULL) {
                return;
            }
            p = next;
        }

        p = wc_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            out->ok = 1;
            return;
        }
        return;                /* unexpected trailing junk */
    }
}

/* ------------------------------------------------------------------ */
/* PASS/FAIL accounting for the network scenario checks                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *label)
{
    if (cond) {
        printf("[ PASS ] %s\n", label);
        g_pass++;
    } else {
        printf("[ FAIL ] %s\n", label);
        g_fail++;
    }
}

/* ------------------------------------------------------------------ */
/* Ready-message validation (contract ReadyHandshake)                  */
/* ------------------------------------------------------------------ */

/*
 * find_feature_bool - locate "<key>":true|false inside the features object
 * substring. Returns 1 if the key is present with a boolean value.
 */
static int features_has_key(const char *features, const char *key)
{
    char pat[64];
    const char *hit;

    wsprintfA(pat, "\"%s\"", key);
    hit = strstr(features, pat);
    if (hit == NULL) {
        return 0;
    }
    hit += lstrlenA(pat);
    hit = wc_skip_ws(hit);
    if (*hit != ':') {
        return 0;
    }
    hit = wc_skip_ws(hit + 1);
    return strncmp(hit, "true", 4) == 0 || strncmp(hit, "false", 5) == 0;
}

/*
 * validate_ready - returns 1 if line satisfies the ReadyHandshake
 * contract (rule ReadyReceived shape; ReadyShape invariant), 0 if it is
 * malformed/not-ready (rule ReadyMalformed). Extra keys are tolerated.
 */
static int validate_ready(const char *line, char *why, int whySize)
{
    char        status[64];
    char        version[WC_VAL_MAX];
    char        transport[WC_VAL_MAX];
    const char *fstart;
    const char *fend;
    char        features[WC_LINE_MAX];
    const char *codepage_kv;
    int         codepage;
    const char *endp;
    int         i;
    static const char *docKeys[8] = {
        "is_win32s", "is_win9x", "is_nt", "is_wow64",
        "threads", "job_objects", "ctrl_events", "pty"
    };

    /* status == "ready" */
    {
        const char *s = strstr(line, "\"status\"");
        const char *vp;
        if (s == NULL) {
            lstrcpynA(why, "no status key", whySize);
            return 0;
        }
        vp = wc_skip_ws(s + lstrlenA("\"status\""));
        if (*vp != ':') {
            lstrcpynA(why, "status not a field", whySize);
            return 0;
        }
        vp = wc_skip_ws(vp + 1);
        if (wc_parse_string(vp, status, (int)sizeof(status)) == NULL ||
            strcmp(status, "ready") != 0) {
            lstrcpynA(why, "status != ready", whySize);
            return 0;
        }
    }

    /* codepage integer present */
    codepage_kv = strstr(line, "\"codepage\"");
    if (codepage_kv == NULL) {
        lstrcpynA(why, "no codepage", whySize);
        return 0;
    }
    codepage_kv = wc_skip_ws(codepage_kv + lstrlenA("\"codepage\""));
    if (*codepage_kv != ':' ||
        !wc_parse_int(codepage_kv + 1, &codepage, &endp)) {
        lstrcpynA(why, "codepage not an integer", whySize);
        return 0;
    }

    /* version non-empty string */
    {
        const char *v = strstr(line, "\"version\"");
        const char *vp;
        if (v == NULL) {
            lstrcpynA(why, "no version", whySize);
            return 0;
        }
        vp = wc_skip_ws(v + lstrlenA("\"version\""));
        if (*vp != ':') {
            lstrcpynA(why, "version not a field", whySize);
            return 0;
        }
        vp = wc_skip_ws(vp + 1);
        if (wc_parse_string(vp, version, (int)sizeof(version)) == NULL ||
            version[0] == '\0') {
            lstrcpynA(why, "version empty", whySize);  /* ReadySessionsHaveVersion */
            return 0;
        }
    }

    /* transport non-empty string */
    {
        const char *t = strstr(line, "\"transport\"");
        const char *vp;
        if (t == NULL) {
            lstrcpynA(why, "no transport", whySize);
            return 0;
        }
        vp = wc_skip_ws(t + lstrlenA("\"transport\""));
        if (*vp != ':') {
            lstrcpynA(why, "transport not a field", whySize);
            return 0;
        }
        vp = wc_skip_ws(vp + 1);
        if (wc_parse_string(vp, transport, (int)sizeof(transport)) == NULL ||
            transport[0] == '\0') {
            lstrcpynA(why, "transport empty", whySize);
            return 0;
        }
    }

    /* features object with the eight documented boolean keys */
    fstart = strstr(line, "\"features\"");
    if (fstart == NULL) {
        lstrcpynA(why, "no features", whySize);
        return 0;
    }
    fstart = strchr(fstart, '{');
    if (fstart == NULL) {
        lstrcpynA(why, "features not an object", whySize);
        return 0;
    }
    fend = wc_skip_value(fstart);
    if (fend == NULL || fend - fstart >= (int)sizeof(features)) {
        lstrcpynA(why, "features unbalanced", whySize);
        return 0;
    }
    {
        int flen = (int)(fend - fstart);
        memcpy(features, fstart, flen);
        features[flen] = '\0';
    }
    for (i = 0; i < 8; i++) {
        if (!features_has_key(features, docKeys[i])) {
            wsprintfA(why, "features missing %s", docKeys[i]);
            return 0;
        }
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* Network plumbing (Winsock 1.1)                                      */
/* ------------------------------------------------------------------ */

static char  g_rxbuf[WC_LINE_MAX * 4];
static int   g_rxlen = 0;

/*
 * recv_line - newline-framed read from the socket. Returns 1 and fills
 * line (without the newline) on success, 0 on connection close/error.
 */
static int recv_line(SOCKET s, char *line, int lineSize)
{
    int i;

    for (;;) {
        for (i = 0; i < g_rxlen; i++) {
            if (g_rxbuf[i] == '\n') {
                int copy = i;
                if (copy >= lineSize) {
                    copy = lineSize - 1;
                }
                memcpy(line, g_rxbuf, copy);
                line[copy] = '\0';
                memmove(g_rxbuf, g_rxbuf + i + 1, g_rxlen - i - 1);
                g_rxlen -= i + 1;
                return 1;
            }
        }
        if (g_rxlen >= (int)sizeof(g_rxbuf)) {
            return 0;          /* overlong line; give up */
        }
        {
            int n = recv(s, g_rxbuf + g_rxlen,
                         (int)sizeof(g_rxbuf) - g_rxlen, 0);
            if (n <= 0) {
                return 0;
            }
            g_rxlen += n;
        }
    }
}

static int send_all(SOCKET s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) {
            return 0;
        }
        sent += n;
    }
    return 1;
}

/*
 * round_trip - send one request line, read the response, parse it.
 * Returns 1 if a response was read and parsed, 0 on transport failure.
 */
static int round_trip(SOCKET s, const char *request, WireResponse *resp)
{
    char line[WC_LINE_MAX];

    if (!send_all(s, request, lstrlenA(request))) {
        return 0;
    }
    if (!recv_line(s, line, (int)sizeof(line))) {
        return 0;
    }
    wire_parse_response(line, resp);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Property-based tests against wire_parse_response          */
/* ------------------------------------------------------------------ */

static void wc_rand_alnum(prop_ctx *_pc, char *out, int len)
{
    static const char alnum[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    int i;
    for (i = 0; i < len; i++) {
        out[i] = alnum[PROP_INT(0, 61)];
    }
    out[len] = '\0';
}

/*
 * PBT a: correlation ids round-trip. A random alnum id formatted into a
 * valid response line parses back equal.
 * Obligation: rule-success.ResponseCorrelated (PBT row);
 *             invariant.ResponsesMatchTheirRequest.
 */
PROP_TEST(pbt_id_round_trip)
{
    char         id[32];
    char         line[256];
    WireResponse resp;
    int          len = PROP_INT(1, 16);

    wc_rand_alnum(_pc, id, len);
    wsprintfA(line, "{\"id\":\"%s\",\"status\":\"ok\",\"data\":\"x\"}", id);
    wire_parse_response(line, &resp);

    PROP_CHECK(resp.ok == 1);
    PROP_CHECK(strcmp(resp.id, id) == 0);
    PROP_CHECK(strcmp(resp.status, "ok") == 0);
}

/*
 * PBT b: unknown keys ignored. Random extra key/value pairs injected into
 * a valid response line; id/status still extracted correctly.
 * Obligation: RequestResponse @guidance (ignore unknown response keys).
 */
PROP_TEST(pbt_unknown_keys_ignored)
{
    char         id[32];
    char         line[1024];
    char         extra[512];
    WireResponse resp;
    int          nExtra = PROP_INT(0, 4);
    int          k;
    int          len = PROP_INT(1, 16);

    wc_rand_alnum(_pc, id, len);
    extra[0] = '\0';
    for (k = 0; k < nExtra; k++) {
        char kbuf[16];
        char vbuf[16];
        char pair[64];
        int  klen = PROP_INT(1, 8);
        int  vlen = PROP_INT(1, 8);
        int  numeric = PROP_BOOL();

        wc_rand_alnum(_pc, kbuf, klen);
        if (numeric) {
            wsprintfA(pair, ",\"x%s\":%d", kbuf, PROP_INT(0, 99999));
        } else {
            wc_rand_alnum(_pc, vbuf, vlen);
            wsprintfA(pair, ",\"x%s\":\"%s\"", kbuf, vbuf);
        }
        if (lstrlenA(extra) + lstrlenA(pair) < (int)sizeof(extra)) {
            lstrcatA(extra, pair);
        }
    }
    wsprintfA(line, "{\"id\":\"%s\"%s,\"status\":\"ok\"%s}",
              id, extra, extra);
    wire_parse_response(line, &resp);

    PROP_CHECK(resp.ok == 1);
    PROP_CHECK(strcmp(resp.id, id) == 0);
    PROP_CHECK(strcmp(resp.status, "ok") == 0);
}

/*
 * PBT c: malformed responses never crash. Random byte strings (printable
 * + structural specials, len 0..256) into the parser. It must return
 * (ok 0 or 1), never crash, and on ok=0 leave a usable struct.
 * Obligation: RequestResponse @guidance (malformed surfaces as client
 *             error, never a crash).
 */
PROP_TEST(pbt_malformed_never_crashes)
{
    static const char special[] = "{}[]\":,\\ \t\n";
    char         line[260];
    WireResponse resp;
    int          len = PROP_INT(0, 256);
    int          i;

    for (i = 0; i < len; i++) {
        if (PROP_INT(0, 3) == 0) {
            line[i] = special[PROP_INT(0, (int)sizeof(special) - 2)];
        } else {
            line[i] = (char)PROP_INT(0x20, 0x7e);
        }
    }
    line[len] = '\0';

    /* The contract: this call must return without crashing. */
    wire_parse_response(line, &resp);
    PROP_CHECK(resp.ok == 0 || resp.ok == 1);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    WSADATA       wsa;
    SOCKET        sock;
    struct sockaddr_in addr;
    unsigned long inaddr;
    const char   *host;
    int           port;
    int           readyOnly = 0;
    char          line[WC_LINE_MAX];
    char          why[256];
    int           pbtFail;

    if (argc < 3) {
        fprintf(stderr,
                "usage: wire_client <host> <port> [--ready-only]\n");
        return 2;
    }
    host = argv[1];
    port = atoi(argv[2]);
    if (argc >= 4 && strcmp(argv[3], "--ready-only") == 0) {
        readyOnly = 1;
    }

    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        WSACleanup();
        return 2;
    }

    inaddr = inet_addr(host);
    if (inaddr == INADDR_NONE) {
        fprintf(stderr, "bad host address: %s\n", host);
        closesocket(sock);
        WSACleanup();
        return 2;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = inaddr;

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "connect() to %s:%d failed\n", host, port);
        closesocket(sock);
        WSACleanup();
        return 2;
    }

    /* SessionOpened: connection established -> session in 'connected'. */
    printf("--- ready handshake ---\n");

    /* ReadyIsFirst: the FIRST line must be the ready message. */
    if (!recv_line(sock, line, (int)sizeof(line))) {
        printf("[ FAIL ] read ready line\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    /* rule ReadyReceived vs rule ReadyMalformed. */
    if (!validate_ready(line, why, (int)sizeof(why))) {
        printf("[ FAIL ] ready malformed: %s\n", why);
        printf("         line: %s\n", line);
        closesocket(sock);
        WSACleanup();
        return 1;     /* MalformedReadyReceived -> session failed */
    }
    check(1, "ready: shape valid (status/codepage/version/transport/features)");

    if (readyOnly) {
        printf("--- ready-only: done ---\n%d passed, %d failed\n",
               g_pass, g_fail);
        closesocket(sock);
        WSACleanup();
        return g_fail == 0 ? 0 : 1;
    }

    /* --- request/response round trips, sequential --- */
    printf("--- request/response round trips ---\n");

    /* 1. echo round-trip. */
    {
        WireResponse r;
        if (!round_trip(sock,
                "{\"cmd\":\"echo\",\"id\":\"w1\",\"line\":\"ping\"}\n", &r)) {
            check(0, "echo: transport");
        } else {
            check(r.ok && strcmp(r.id, "w1") == 0,
                  "echo: id correlated (w1)");
            check(strcmp(r.status, "ok") == 0, "echo: status ok");
            check(strcmp(r.data, "ping") == 0, "echo: data == ping");
        }
    }

    /* 2. exec round-trip (unsafe bypasses the catalog gate). */
    {
        WireResponse r;
        if (!round_trip(sock,
                "{\"cmd\":\"exec\",\"id\":\"w2\",\"argv\":"
                "[\"cmd\",\"/c\",\"echo\",\"hi\"],\"unsafe\":true}\n", &r)) {
            check(0, "exec: transport");
        } else {
            check(r.ok && strcmp(r.id, "w2") == 0,
                  "exec: id correlated (w2)");
            check(strcmp(r.status, "ok") == 0, "exec: status ok");
            check(r.has_exit_code && r.exit_code == 0,
                  "exec: exit_code 0");
            check(r.has_stdout_b64 && r.stdout_b64[0] != '\0',
                  "exec: stdout_b64 present and non-empty");
        }
    }

    /* 3. error round-trip (unknown command). */
    {
        WireResponse r;
        if (!round_trip(sock,
                "{\"cmd\":\"nope\",\"id\":\"w3\"}\n", &r)) {
            check(0, "error: transport");
        } else {
            check(r.ok && strcmp(r.id, "w3") == 0,
                  "error: id correlated (w3)");
            check(strcmp(r.status, "error") == 0, "error: status error");
        }
    }

    closesocket(sock);
    WSACleanup();

    /* --- in-process PBT against the response parser --- */
    printf("--- parser property-based tests ---\n");
    prop_seed(0x5712C0DEUL);   /* fixed seed for reproducibility */
    PROP_RUN(pbt_id_round_trip,          250);
    PROP_RUN(pbt_unknown_keys_ignored,   250);
    PROP_RUN(pbt_malformed_never_crashes, 300);
    pbtFail = prop_summary();

    printf("=== wire_client summary: %d passed, %d failed (scenario); "
           "PBT %s ===\n",
           g_pass, g_fail, pbtFail == 0 ? "green" : "RED");

    return (g_fail == 0 && pbtFail == 0) ? 0 : 1;
}
