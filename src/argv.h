/*
 * argv.h - argv array -> CreateProcessA command line for MCP-Win32s
 *
 * Implements the reverse of the CommandLineToArgvW quoting rules so that
 * a child spawned via CreateProcessA(lpApplicationName=NULL, line) sees
 * exactly the argv vector we joined (Q8 in plan/PHASE4.md). When the
 * request runs through cmd.exe (shell:true), ArgvCmdEscape additionally
 * ^-escapes the cmd metacharacters that survive the first pass (Q15).
 *
 * Reference: Daniel Colascione, "Everyone quotes command line arguments
 * the wrong way" (MSDN blog, 2011).
 *
 * C89. No floating point. ANSI/DBCS-safe (lead bytes pass through verbatim).
 *
 * Return convention (all three functions):
 *   >= 0  number of characters written to out, EXCLUDING the NUL terminator
 *   -1    out buffer too small (out is left in an unspecified state)
 * out is always NUL-terminated on success.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef ARGV_H
#define ARGV_H

/*
 * ArgvEscapeArg - Escape one argument per CommandLineToArgvW reverse-rules.
 *
 * Empty arg          -> "" (two double quotes).
 * No [ \t\n\v"] char -> emitted verbatim.
 * Otherwise          -> wrapped in double quotes with backslash-run rules:
 *   a run of N backslashes before a '"' becomes 2N+1 backslashes + \" ;
 *   a run of N backslashes before the closing quote becomes 2N backslashes.
 *
 * Returns chars written (excluding NUL), or -1 on overflow.
 */
int ArgvEscapeArg(const char *arg, char *out, int outSize);

/*
 * ArgvJoin - Escape each of argv[0..argc) and join with single spaces.
 *
 * Returns chars written (excluding NUL), or -1 on overflow.
 * argc == 0 yields an empty string (returns 0).
 */
int ArgvJoin(const char **argv, int argc, char *out, int outSize);

/*
 * ArgvCmdEscape - Prefix cmd.exe metacharacters with '^' OUTSIDE quotes.
 *
 * Applied to an already-joined line when shell:true, so cmd.exe does not
 * interpret & | < > ^ ( ) % as syntax. Characters inside double-quoted
 * regions are left untouched (cmd does not treat them as metacharacters
 * there). A '^' that precedes an end-of-line is still emitted as "^^"
 * defensively, matching how cmd consumes a trailing caret.
 *
 * Returns chars written (excluding NUL), or -1 on overflow.
 */
int ArgvCmdEscape(const char *line, char *out, int outSize);

#endif /* ARGV_H */
