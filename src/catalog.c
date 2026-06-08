/*
 * catalog.c - Command catalog loader and whitelist (Phase 4)
 *
 * The shipped json_parser.c is single-level (flat key/value objects) and
 * cannot parse catalog/win32-commands.json, which nests objects, arrays,
 * and objects-within-arrays. This file therefore carries its own small
 * recursive-descent scanner (C89, fixed-size storage, no floating point).
 * Only the fields the server needs are retained; all other JSON values are
 * skipped over without being stored.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "catalog.h"
#include "json_parser.h"

#define CATALOG_MAX_ENTRIES 64
#define CATALOG_MAX_OPTIONS 16
#define CATALOG_NAME_LEN    64
#define CATALOG_FLAG_LEN    32
#define CATALOG_SHELL_LEN   128
#define CATALOG_FILE_MAX    65536

typedef struct {
    char flag[CATALOG_FLAG_LEN];
    int  has_arg;
} CatalogOption;

struct CatalogEntry {
    char name[CATALOG_NAME_LEN];
    int  is_builtin;
    int  supports_win32s;
    char shell_modern[CATALOG_SHELL_LEN];
    char shell_win32s[CATALOG_SHELL_LEN];
    CatalogOption options[CATALOG_MAX_OPTIONS];
    int  option_count;
};

struct Catalog {
    CatalogEntry entries[CATALOG_MAX_ENTRIES];
    int          entry_count;
};

/* ---- Scanner state ---- */

typedef struct {
    const char *p;
    const char *end;
    char       *err;
    int         errSize;
} Scanner;

static void scanError(Scanner *s, const char *msg)
{
    if (s->err != NULL && s->errSize > 0) {
        lstrcpynA(s->err, msg, s->errSize);
    }
}

static void skipWs(Scanner *s)
{
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s->p++;
        } else {
            break;
        }
    }
}

/*
 * scanString - Parse a JSON string starting at the opening quote.
 * Copies up to outSize-1 chars (out may be NULL to discard). Handles the
 * escape sequences that appear in the catalog (\\ \" \/ \n \r \t).
 * Returns 1 on success, 0 on malformed input.
 */
static int scanString(Scanner *s, char *out, int outSize)
{
    int n;

    skipWs(s);
    if (s->p >= s->end || *s->p != '"') {
        scanError(s, "expected string");
        return 0;
    }
    s->p++;
    n = 0;
    while (s->p < s->end) {
        char c = *s->p++;
        if (c == '"') {
            if (out != NULL && outSize > 0) {
                if (n < outSize) {
                    out[n] = '\0';
                } else {
                    out[outSize - 1] = '\0';
                }
            }
            return 1;
        }
        if (c == '\\') {
            if (s->p >= s->end) {
                break;
            }
            {
                char e = *s->p++;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case '"': c = '"'; break;
                    case '\\': c = '\\'; break;
                    case '/': c = '/'; break;
                    default: c = e; break;
                }
            }
        }
        if (out != NULL && n < outSize - 1) {
            out[n] = c;
        }
        n++;
    }
    scanError(s, "unterminated string");
    return 0;
}

/* Forward declaration for recursive skipping. */
static int skipValue(Scanner *s);

static int skipObject(Scanner *s)
{
    skipWs(s);
    if (s->p >= s->end || *s->p != '{') {
        scanError(s, "expected object");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return 1;
    }
    for (;;) {
        if (!scanString(s, NULL, 0)) {
            return 0;
        }
        skipWs(s);
        if (s->p >= s->end || *s->p != ':') {
            scanError(s, "expected ':'");
            return 0;
        }
        s->p++;
        if (!skipValue(s)) {
            return 0;
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or '}'");
        return 0;
    }
}

static int skipArray(Scanner *s)
{
    skipWs(s);
    if (s->p >= s->end || *s->p != '[') {
        scanError(s, "expected array");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == ']') {
        s->p++;
        return 1;
    }
    for (;;) {
        if (!skipValue(s)) {
            return 0;
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or ']'");
        return 0;
    }
}

/*
 * skipLiteral - Skip a number, true, false, or null token.
 */
static int skipLiteral(Scanner *s)
{
    skipWs(s);
    if (s->p >= s->end) {
        scanError(s, "unexpected end of input");
        return 0;
    }
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ',' || c == '}' || c == ']' ||
            c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            break;
        }
        s->p++;
    }
    return 1;
}

static int skipValue(Scanner *s)
{
    skipWs(s);
    if (s->p >= s->end) {
        scanError(s, "unexpected end of input");
        return 0;
    }
    switch (*s->p) {
        case '"': return scanString(s, NULL, 0);
        case '{': return skipObject(s);
        case '[': return skipArray(s);
        default:  return skipLiteral(s);
    }
}

/*
 * parseOption - Parse one option object: {"flag":"/A","arg":"x","desc":"..."}.
 * Records flag and whether an "arg" key is present; skips everything else.
 */
static int parseOption(Scanner *s, CatalogOption *opt)
{
    opt->flag[0] = '\0';
    opt->has_arg = 0;

    skipWs(s);
    if (s->p >= s->end || *s->p != '{') {
        scanError(s, "expected option object");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return 1;
    }
    for (;;) {
        char key[32];
        if (!scanString(s, key, sizeof(key))) {
            return 0;
        }
        skipWs(s);
        if (s->p >= s->end || *s->p != ':') {
            scanError(s, "expected ':'");
            return 0;
        }
        s->p++;
        if (lstrcmpA(key, "flag") == 0) {
            if (!scanString(s, opt->flag, sizeof(opt->flag))) {
                return 0;
            }
        } else if (lstrcmpA(key, "arg") == 0) {
            opt->has_arg = 1;
            if (!skipValue(s)) {
                return 0;
            }
        } else {
            if (!skipValue(s)) {
                return 0;
            }
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or '}'");
        return 0;
    }
}

/*
 * parseOptions - Parse an "options" array into the entry.
 */
static int parseOptions(Scanner *s, CatalogEntry *e)
{
    skipWs(s);
    if (s->p >= s->end || *s->p != '[') {
        scanError(s, "expected options array");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == ']') {
        s->p++;
        return 1;
    }
    for (;;) {
        if (e->option_count < CATALOG_MAX_OPTIONS) {
            if (!parseOption(s, &e->options[e->option_count])) {
                return 0;
            }
            e->option_count++;
        } else {
            /* Too many options: skip the rest without storing. */
            if (!skipValue(s)) {
                return 0;
            }
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or ']'");
        return 0;
    }
}

/*
 * parseEntry - Parse one command entry object.
 */
static int parseEntry(Scanner *s, CatalogEntry *e)
{
    e->is_builtin = 0;
    e->supports_win32s = 0;
    e->shell_modern[0] = '\0';
    e->shell_win32s[0] = '\0';
    e->option_count = 0;

    skipWs(s);
    if (s->p >= s->end || *s->p != '{') {
        scanError(s, "expected entry object");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return 1;
    }
    for (;;) {
        char key[32];
        if (!scanString(s, key, sizeof(key))) {
            return 0;
        }
        skipWs(s);
        if (s->p >= s->end || *s->p != ':') {
            scanError(s, "expected ':'");
            return 0;
        }
        s->p++;
        if (lstrcmpA(key, "kind") == 0) {
            char kind[32];
            if (!scanString(s, kind, sizeof(kind))) {
                return 0;
            }
            e->is_builtin = (lstrcmpA(kind, "shell-builtin") == 0);
        } else if (lstrcmpA(key, "shell_modern") == 0) {
            if (!scanString(s, e->shell_modern, sizeof(e->shell_modern))) {
                return 0;
            }
        } else if (lstrcmpA(key, "shell_win32s") == 0) {
            if (!scanString(s, e->shell_win32s, sizeof(e->shell_win32s))) {
                return 0;
            }
        } else if (lstrcmpA(key, "supports_win32s") == 0) {
            skipWs(s);
            if (s->p < s->end && *s->p == 't') {
                e->supports_win32s = 1;
            }
            if (!skipLiteral(s)) {
                return 0;
            }
        } else if (lstrcmpA(key, "options") == 0) {
            if (!parseOptions(s, e)) {
                return 0;
            }
        } else {
            if (!skipValue(s)) {
                return 0;
            }
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or '}'");
        return 0;
    }
}

/*
 * parseCommands - Parse the "commands" object: keyed entries.
 */
static int parseCommands(Scanner *s, Catalog *cat)
{
    skipWs(s);
    if (s->p >= s->end || *s->p != '{') {
        scanError(s, "expected commands object");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return 1;
    }
    for (;;) {
        char name[CATALOG_NAME_LEN];
        if (!scanString(s, name, sizeof(name))) {
            return 0;
        }
        skipWs(s);
        if (s->p >= s->end || *s->p != ':') {
            scanError(s, "expected ':'");
            return 0;
        }
        s->p++;
        if (cat->entry_count < CATALOG_MAX_ENTRIES) {
            CatalogEntry *e = &cat->entries[cat->entry_count];
            lstrcpynA(e->name, name, sizeof(e->name));
            if (!parseEntry(s, e)) {
                return 0;
            }
            cat->entry_count++;
        } else {
            scanError(s, "too many catalog entries");
            return 0;
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or '}'");
        return 0;
    }
}

/*
 * parseTop - Parse the top-level document: {"version":1,"commands":{...}}.
 */
static int parseTop(Scanner *s, Catalog *cat)
{
    skipWs(s);
    if (s->p >= s->end || *s->p != '{') {
        scanError(s, "expected top-level object");
        return 0;
    }
    s->p++;
    skipWs(s);
    if (s->p < s->end && *s->p == '}') {
        return 1;
    }
    for (;;) {
        char key[32];
        if (!scanString(s, key, sizeof(key))) {
            return 0;
        }
        skipWs(s);
        if (s->p >= s->end || *s->p != ':') {
            scanError(s, "expected ':'");
            return 0;
        }
        s->p++;
        if (lstrcmpA(key, "commands") == 0) {
            if (!parseCommands(s, cat)) {
                return 0;
            }
        } else {
            if (!skipValue(s)) {
                return 0;
            }
        }
        skipWs(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return 1;
        }
        scanError(s, "expected ',' or '}'");
        return 0;
    }
}

/*
 * readFile - Read an entire file into buf via CreateFileA/ReadFile.
 * Returns the byte count, or -1 on failure.
 */
static int readFile(const char *path, char *buf, int bufSize)
{
    HANDLE hFile;
    DWORD  read;
    DWORD  total;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return -1;
    }
    total = 0;
    for (;;) {
        if ((int)total >= bufSize - 1) {
            CloseHandle(hFile);
            return -1;
        }
        if (!ReadFile(hFile, buf + total, (DWORD)(bufSize - 1 - total),
                      &read, NULL)) {
            CloseHandle(hFile);
            return -1;
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    CloseHandle(hFile);
    buf[total] = '\0';
    return (int)total;
}

int CatalogLoad(const char *path, Catalog **outCat, char *errMsg, int errSize)
{
    Catalog *cat;
    char    *fileBuf;
    int      len;
    Scanner  s;

    if (outCat != NULL) {
        *outCat = NULL;
    }
    if (errSize > 0 && errMsg != NULL) {
        errMsg[0] = '\0';
    }

    fileBuf = (char *)malloc(CATALOG_FILE_MAX);
    if (fileBuf == NULL) {
        if (errMsg != NULL) {
            lstrcpynA(errMsg, "out of memory", errSize);
        }
        return 0;
    }

    len = readFile(path, fileBuf, CATALOG_FILE_MAX);
    if (len < 0) {
        free(fileBuf);
        if (errMsg != NULL) {
            lstrcpynA(errMsg, "catalog file not found or unreadable", errSize);
        }
        return 0;
    }

    cat = (Catalog *)malloc(sizeof(Catalog));
    if (cat == NULL) {
        free(fileBuf);
        if (errMsg != NULL) {
            lstrcpynA(errMsg, "out of memory", errSize);
        }
        return 0;
    }
    cat->entry_count = 0;

    s.p = fileBuf;
    s.end = fileBuf + len;
    s.err = errMsg;
    s.errSize = errSize;

    if (!parseTop(&s, cat)) {
        free(fileBuf);
        free(cat);
        return 0;
    }

    /* A whitelist with nothing whitelisted is a load failure, not a
     * loaded catalog (spec: catalog.allium invariant
     * LoadedCatalogHasEntries; weed 2026-06-06). */
    if (cat->entry_count == 0) {
        free(fileBuf);
        free(cat);
        if (errMsg != NULL) {
            lstrcpynA(errMsg, "no commands in catalog", errSize);
        }
        return 0;
    }

    free(fileBuf);
    *outCat = cat;
    return 1;
}

void CatalogFree(Catalog *cat)
{
    if (cat != NULL) {
        free(cat);
    }
}

const CatalogEntry *CatalogLookup(const Catalog *cat, const char *cmdName)
{
    int i;

    if (cat == NULL || cmdName == NULL) {
        return NULL;
    }
    for (i = 0; i < cat->entry_count; i++) {
        if (lstrcmpiA(cat->entries[i].name, cmdName) == 0) {
            return &cat->entries[i];
        }
    }
    return NULL;
}

/*
 * isFlagToken - A token is a flag when it starts with '/' or '-'.
 */
static int isFlagToken(const char *tok)
{
    return tok != NULL && (tok[0] == '/' || tok[0] == '-');
}

/*
 * findOption - Case-insensitive match of a flag token against the entry's
 * options. The token may be glued (/A:value or /Fename): the option's flag
 * is matched as a prefix when the option declares an arg, otherwise it must
 * match exactly. Returns the option, or NULL.
 */
static const CatalogOption *findOption(const CatalogEntry *e, const char *tok)
{
    int i;
    int tokLen;

    tokLen = lstrlenA(tok);
    for (i = 0; i < e->option_count; i++) {
        const CatalogOption *opt = &e->options[i];
        int flagLen = lstrlenA(opt->flag);

        if (lstrcmpiA(opt->flag, tok) == 0) {
            return opt;
        }
        /* Glued form: option flag is a prefix of the token, and what
         * follows is a separator or value. Only meaningful for arg flags. */
        if (opt->has_arg && flagLen > 0 && tokLen > flagLen) {
            char head[CATALOG_FLAG_LEN];
            int n = flagLen;
            if (n >= (int)sizeof(head)) {
                n = (int)sizeof(head) - 1;
            }
            memcpy(head, tok, (size_t)n);
            head[n] = '\0';
            if (lstrcmpiA(head, opt->flag) == 0) {
                return opt;
            }
        }
    }
    return NULL;
}

int CatalogValidateArgs(const CatalogEntry *entry, const char **argv,
                        int argc, char *errMsg, int errSize)
{
    int i;

    if (errSize > 0 && errMsg != NULL) {
        errMsg[0] = '\0';
    }
    if (entry == NULL) {
        if (errMsg != NULL) {
            lstrcpynA(errMsg, "no catalog entry", errSize);
        }
        return 0;
    }

    /* argv[0] is the command name; scan the remaining tokens. */
    for (i = 1; i < argc; i++) {
        const char *tok = argv[i];
        if (tok == NULL) {
            continue;
        }
        if (isFlagToken(tok)) {
            const CatalogOption *opt = findOption(entry, tok);
            if (opt == NULL) {
                if (errMsg != NULL) {
                    lstrcpynA(errMsg, "argument not allowed", errSize);
                }
                return 0;
            }
            /* Split form: an arg flag consumes the next token when that
             * token is not itself a flag and the flag was not glued. */
            if (opt->has_arg && lstrcmpiA(opt->flag, tok) == 0) {
                if (i + 1 < argc && !isFlagToken(argv[i + 1])) {
                    i++;
                }
            }
        }
        /* Non-flag tokens are positionals: always allowed (advisory). */
    }
    return 1;
}

const char *CatalogEntryName(const CatalogEntry *e)
{
    return e != NULL ? e->name : NULL;
}

int CatalogEntryIsBuiltin(const CatalogEntry *e)
{
    return e != NULL ? e->is_builtin : 0;
}

const char *CatalogEntryShellModern(const CatalogEntry *e)
{
    return e != NULL ? e->shell_modern : NULL;
}

const char *CatalogEntryShellWin32s(const CatalogEntry *e)
{
    return e != NULL ? e->shell_win32s : NULL;
}

int CatalogEntrySupportsWin32s(const CatalogEntry *e)
{
    return e != NULL ? e->supports_win32s : 0;
}

int CatalogCount(const Catalog *cat)
{
    return cat != NULL ? cat->entry_count : 0;
}

/*
 * serialAppend - Bounds-checked append of a literal/string to out at *pos.
 * Returns 1 on success, 0 if it would overflow (out unchanged at *pos).
 */
static int serialAppend(char *out, int outSize, int *pos, const char *src)
{
    int len;
    int i;

    len = lstrlenA(src);
    if (*pos + len >= outSize) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        out[*pos + i] = src[i];
    }
    *pos += len;
    out[*pos] = '\0';
    return 1;
}

/*
 * serialAppendEscaped - JSON-escape src (via the shared JsonEscape helper)
 * and append the result to out at *pos. Returns 1 on success, 0 on overflow.
 */
static int serialAppendEscaped(char *out, int outSize, int *pos,
                               const char *src)
{
    /* Worst case: a CATALOG_NAME_LEN field of all-C0 bytes, each escaped to
       \u00XX (6x); flag fields (CATALOG_FLAG_LEN) are shorter, so name bounds
       it. (The old CATALOG_SHELL_LEN*2 sizing predated the C0 \u-escape and
       was both the wrong field and under-provisioned.) */
    char esc[CATALOG_NAME_LEN * 6 + 2];

    if (JsonEscape(src != NULL ? src : "", esc, (int)sizeof(esc)) < 0) {
        return 0;
    }
    return serialAppend(out, outSize, pos, esc);
}

int CatalogSerializeJson(const Catalog *cat, char *out, int outSize)
{
    int pos;
    int i;

    if (out == NULL || outSize < 1) {
        return -1;
    }
    out[0] = '\0';
    pos = 0;

    if (!serialAppend(out, outSize, &pos, "[")) {
        out[0] = '\0';
        return -1;
    }
    if (cat != NULL) {
        for (i = 0; i < cat->entry_count; i++) {
            const CatalogEntry *e = &cat->entries[i];
            int j;

            if (i > 0) {
                if (!serialAppend(out, outSize, &pos, ",")) {
                    out[0] = '\0';
                    return -1;
                }
            }
            /* name (escaped); description is wire-shape only - not retained
             * in CatalogEntry, so it serialises empty; builtin from is_builtin;
             * destructive has no struct field, so it is constant false. */
            if (!serialAppend(out, outSize, &pos, "{\"name\":\"") ||
                !serialAppendEscaped(out, outSize, &pos, e->name) ||
                !serialAppend(out, outSize, &pos, "\",\"description\":\"\",") ||
                !serialAppend(out, outSize, &pos, "\"builtin\":") ||
                !serialAppend(out, outSize, &pos,
                              e->is_builtin ? "true" : "false") ||
                !serialAppend(out, outSize, &pos,
                              ",\"destructive\":false,\"flags\":[")) {
                out[0] = '\0';
                return -1;
            }
            for (j = 0; j < e->option_count; j++) {
                const CatalogOption *opt = &e->options[j];

                if (j > 0) {
                    if (!serialAppend(out, outSize, &pos, ",")) {
                        out[0] = '\0';
                        return -1;
                    }
                }
                /* flag (escaped); takes_value from has_arg; the per-flag
                 * description is wire-shape only - not retained, so empty. */
                if (!serialAppend(out, outSize, &pos, "{\"flag\":\"") ||
                    !serialAppendEscaped(out, outSize, &pos, opt->flag) ||
                    !serialAppend(out, outSize, &pos, "\",\"takes_value\":") ||
                    !serialAppend(out, outSize, &pos,
                                  opt->has_arg ? "true" : "false") ||
                    !serialAppend(out, outSize, &pos,
                                  ",\"description\":\"\"}")) {
                    out[0] = '\0';
                    return -1;
                }
            }
            if (!serialAppend(out, outSize, &pos, "]}")) {
                out[0] = '\0';
                return -1;
            }
        }
    }
    if (!serialAppend(out, outSize, &pos, "]")) {
        out[0] = '\0';
        return -1;
    }
    return pos;
}
