/*
 * argv.c - argv array -> CreateProcessA command line for MCP-Win32s
 *
 * See argv.h for the public contract and the references. The quoting is the
 * exact reverse of CommandLineToArgvW's tokenizer (Q8 in plan/PHASE4.md);
 * ArgvCmdEscape adds the cmd.exe ^-escape pass (Q15).
 *
 * C89. No floating point. ANSI string handling only (no wchar_t).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include "argv.h"

/*
 * A character that, unquoted, makes CommandLineToArgvW split the token or
 * treat the quote specially: space, tab, newline, vertical tab, double quote.
 */
static int ArgvNeedsQuoting(const char *arg)
{
    const char *p;

    if (arg[0] == '\0') {
        return 1;   /* empty -> must be emitted as "" */
    }
    for (p = arg; *p != '\0'; p++) {
        char c;
        c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
            return 1;
        }
    }
    return 0;
}

/*
 * Append one char to out at *pos, bounded by outSize (which must leave room
 * for a later NUL terminator: callers reserve it). Returns 0 on overflow.
 */
static int ArgvPut(char *out, int *pos, int outSize, char c)
{
    if (*pos >= outSize - 1) {
        return 0;
    }
    out[*pos] = c;
    (*pos)++;
    return 1;
}

int ArgvEscapeArg(const char *arg, char *out, int outSize)
{
    int pos;
    const char *p;

    pos = 0;
    if (outSize < 1) {
        return -1;
    }

    if (!ArgvNeedsQuoting(arg)) {
        for (p = arg; *p != '\0'; p++) {
            if (!ArgvPut(out, &pos, outSize, *p)) {
                return -1;
            }
        }
        out[pos] = '\0';
        return pos;
    }

    if (!ArgvPut(out, &pos, outSize, '"')) {
        return -1;
    }

    p = arg;
    while (*p != '\0') {
        int slashes;

        slashes = 0;
        while (*p == '\\') {
            slashes++;
            p++;
        }

        if (*p == '\0') {
            /* Trailing backslash run: double it, then the closing quote. */
            int i;
            for (i = 0; i < slashes * 2; i++) {
                if (!ArgvPut(out, &pos, outSize, '\\')) {
                    return -1;
                }
            }
            break;
        } else if (*p == '"') {
            /* Run before a quote: emit 2N+1 backslashes, then \". */
            int i;
            for (i = 0; i < slashes * 2 + 1; i++) {
                if (!ArgvPut(out, &pos, outSize, '\\')) {
                    return -1;
                }
            }
            if (!ArgvPut(out, &pos, outSize, '"')) {
                return -1;
            }
            p++;
        } else {
            /* Run before an ordinary byte: emit N backslashes, then the byte.
             * DBCS lead bytes and trail bytes are ordinary here and pass
             * through unsplit. */
            int i;
            for (i = 0; i < slashes; i++) {
                if (!ArgvPut(out, &pos, outSize, '\\')) {
                    return -1;
                }
            }
            if (!ArgvPut(out, &pos, outSize, *p)) {
                return -1;
            }
            p++;
        }
    }

    if (!ArgvPut(out, &pos, outSize, '"')) {
        return -1;
    }
    out[pos] = '\0';
    return pos;
}

int ArgvJoin(const char **argv, int argc, char *out, int outSize)
{
    int pos;
    int i;

    pos = 0;
    if (outSize < 1) {
        return -1;
    }
    out[0] = '\0';

    for (i = 0; i < argc; i++) {
        int n;

        if (i > 0) {
            if (!ArgvPut(out, &pos, outSize, ' ')) {
                return -1;
            }
        }
        /* Escape directly into the tail of out. */
        n = ArgvEscapeArg(argv[i], out + pos, outSize - pos);
        if (n < 0) {
            return -1;
        }
        pos += n;
    }

    out[pos] = '\0';
    return pos;
}

int ArgvCmdEscape(const char *line, char *out, int outSize)
{
    int pos;
    int inQuotes;
    const char *p;

    pos = 0;
    inQuotes = 0;
    if (outSize < 1) {
        return -1;
    }

    for (p = line; *p != '\0'; p++) {
        char c;
        int escape;

        c = *p;
        if (c == '"') {
            inQuotes = !inQuotes;
            if (!ArgvPut(out, &pos, outSize, c)) {
                return -1;
            }
            continue;
        }

        escape = 0;
        if (!inQuotes) {
            switch (c) {
            case '&': case '|': case '<': case '>':
            case '^': case '(': case ')': case '%':
                escape = 1;
                break;
            default:
                break;
            }
        }

        if (escape) {
            if (!ArgvPut(out, &pos, outSize, '^')) {
                return -1;
            }
        }
        if (!ArgvPut(out, &pos, outSize, c)) {
            return -1;
        }
    }

    out[pos] = '\0';
    return pos;
}
