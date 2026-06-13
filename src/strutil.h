/*
 * strutil.h - small string helpers the native-Win32 floor lacks.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */
#ifndef MCP_STRUTIL_H
#define MCP_STRUTIL_H

/*
 * McpStrCpyN - bounded string copy with lstrcpynA semantics: copy at most
 * (n-1) bytes from src to dst and always NUL-terminate when n > 0, never
 * splitting a DBCS character. Returns dst.
 *
 * Supplied in the binary because Windows NT 3.1's kernel32 - the native-Win32
 * floor (1993) - does not export lstrcpynA (it arrived in NT 3.51/Win95). A
 * static lstrcpynA import makes the device fail to load on NT 3.1 with
 * ERROR_BAD_FORMAT/entry-point-not-found. (See plan/PHASE6.md in the host
 * repo, NT 3.1 floor validation.)
 */
char *McpStrCpyN(char *dst, const char *src, int n);

#endif /* MCP_STRUTIL_H */
