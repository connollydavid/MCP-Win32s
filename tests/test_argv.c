/*
 * test_argv.c - Unit + property tests for src/argv.c
 *
 * Covers the 12 fixed cases from plan/PHASE4.md ("argv.c quoting algorithm"
 * / Tests section) plus a 1000-trial property-based roundtrip: a random argv
 * is joined with ArgvJoin, spawned via CreateProcessA into argv_echo.exe, and
 * the child's reported argv (base64 over a pipe) is checked byte-for-byte
 * against the input. These support the exec_ops obligations in
 * tests/OBLIGATIONS-PHASE4.md (argv quoting feeds ProcessSpawned's cmd_line);
 * the cmd-escape cases also exercise Q15.
 *
 * The fixed-case expected strings follow the algorithm in PHASE4.md lines
 * 549-560 exactly: an argument with no [ \t\n\v"] byte is emitted verbatim
 * (so a bare trailing backslash is NOT quoted; it round-trips identically).
 *
 * C89. Win32 (CreateProcessA + anonymous pipes) for the PBT only.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PROP_IMPLEMENTATION
#include "prop.h"

#include "test_framework.h"
#include "argv.h"
#include "base64.h"

/* ========================================================
 * Fixed cases - ArgvEscapeArg / ArgvJoin
 * ======================================================== */

TEST_CASE(escape_simple_verbatim) {
    char out[64];
    int n;
    n = ArgvEscapeArg("a", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("a", out, "no special chars -> verbatim");
}

TEST_CASE(join_two_words) {
    const char *argv[2];
    char out[64];
    int n;
    argv[0] = "a";
    argv[1] = "b";
    n = ArgvJoin(argv, 2, out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("a b", out, "[a,b] -> a b");
}

TEST_CASE(escape_embedded_space) {
    char out[64];
    int n;
    n = ArgvEscapeArg("hello world", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    /* "hello world" */
    TEST_ASSERT_STR_EQUAL("\"hello world\"", out, "space -> quoted");
}

TEST_CASE(escape_embedded_quote) {
    char out[64];
    int n;
    /* arg is the 3 bytes a " b */
    n = ArgvEscapeArg("a\"b", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    /* "a\"b"  -> bytes: " a \ " b " */
    TEST_ASSERT_STR_EQUAL("\"a\\\"b\"", out, "embedded quote -> backslash-quote");
}

TEST_CASE(escape_trailing_backslash_verbatim) {
    char out[64];
    int n;
    /* arg is the 2 bytes a \  -- no whitespace/quote -> verbatim */
    n = ArgvEscapeArg("a\\", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("a\\", out, "trailing backslash, no quoting needed");
}

TEST_CASE(escape_interior_backslash_verbatim) {
    char out[64];
    int n;
    /* arg is the 3 bytes a \ b */
    n = ArgvEscapeArg("a\\b", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("a\\b", out, "interior backslash -> verbatim");
}

TEST_CASE(escape_trailing_backslash_before_quote) {
    char out[64];
    int n;
    /* arg is the 3 bytes a \ "  (has a quote -> must quote the arg) */
    n = ArgvEscapeArg("a\\\"", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    /* run of 1 backslash before the quote -> 2*1+1=3 backslashes, then the
     * escaped quote: bytes  " a \ \ \ " "   => "a\\\"" */
    TEST_ASSERT_STR_EQUAL("\"a\\\\\\\"\"", out, "backslash before quote -> 2N+1");
}

TEST_CASE(escape_empty_arg) {
    char out[64];
    int n;
    n = ArgvEscapeArg("", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("\"\"", out, "empty arg -> two quotes");
}

TEST_CASE(join_with_empty_args) {
    const char *argv[3];
    char out[64];
    int n;
    argv[0] = "x";
    argv[1] = "";
    argv[2] = "";
    n = ArgvJoin(argv, 3, out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("x \"\" \"\"", out, "[x,'',''] -> x \"\" \"\"");
}

TEST_CASE(metachar_no_shell_quoted_only) {
    char out[64];
    int n;
    /* shell=false: ArgvEscapeArg alone; & is not whitespace/quote -> verbatim */
    n = ArgvEscapeArg("a&b", out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT_STR_EQUAL("a&b", out, "no shell: & passes through ArgvEscapeArg");
}

TEST_CASE(metachar_shell_caret_escaped_outside_quotes) {
    char joined[64];
    char escaped[128];
    const char *argv[1];
    int n;
    /* shell=true pipeline: join, then ArgvCmdEscape. a&b has no whitespace so
     * it is emitted verbatim by ArgvJoin, leaving & unquoted for cmd escape. */
    argv[0] = "a&b";
    n = ArgvJoin(argv, 1, joined, sizeof(joined));
    TEST_ASSERT(n >= 0, "join no overflow");
    TEST_ASSERT_STR_EQUAL("a&b", joined, "join leaves & bare");
    n = ArgvCmdEscape(joined, escaped, sizeof(escaped));
    TEST_ASSERT(n >= 0, "escape no overflow");
    TEST_ASSERT_STR_EQUAL("a^&b", escaped, "shell: & -> ^& outside quotes");
}

TEST_CASE(cmd_escape_inside_quotes_untouched) {
    char escaped[128];
    int n;
    /* metachars inside a double-quoted region are NOT caret-escaped */
    n = ArgvCmdEscape("\"a&b\" c|d", escaped, sizeof(escaped));
    TEST_ASSERT(n >= 0, "no overflow");
    /* inside quotes & stays; outside | -> ^| */
    TEST_ASSERT_STR_EQUAL("\"a&b\" c^|d", escaped, "quoted metachar untouched");
}

TEST_CASE(all_printable_ascii_roundtrips_internally) {
    /* All printable ASCII (0x20-0x7E) in one arg escapes and contains every
     * byte (the quote/backslash get escaped but remain recoverable). */
    char arg[96];
    char out[256];
    int i, n, len;
    len = 0;
    for (i = 0x20; i <= 0x7E; i++) {
        arg[len++] = (char)i;
    }
    arg[len] = '\0';
    n = ArgvEscapeArg(arg, out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    TEST_ASSERT(out[0] == '"', "has whitespace+quote -> wrapped in quotes");
    TEST_ASSERT(out[n - 1] == '"', "closing quote present");
}

TEST_CASE(dbcs_lead_bytes_not_split) {
    /* DBCS lead bytes (0x81-0x9F, 0xE0-0xFC) followed by trail bytes must pass
     * through without a backslash being inserted mid-character. We feed a pair
     * containing a backslash-looking trail byte (0x5C == '\\') after a lead
     * byte; verbatim emission keeps the two bytes adjacent. */
    char arg[5];
    char out[32];
    int n;
    arg[0] = (char)0x81;   /* lead byte */
    arg[1] = (char)0x5C;   /* trail byte that equals '\\' */
    arg[2] = (char)0xE0;   /* another lead byte */
    arg[3] = (char)0x40;   /* trail byte '@' */
    arg[4] = '\0';
    n = ArgvEscapeArg(arg, out, sizeof(out));
    TEST_ASSERT(n >= 0, "no overflow");
    /* No whitespace/quote -> verbatim; bytes unchanged and unsplit. */
    TEST_ASSERT_INT_EQUAL(4, n, "four bytes verbatim");
    TEST_ASSERT(out[0] == (char)0x81, "lead byte 1 intact");
    TEST_ASSERT(out[1] == (char)0x5C, "trail byte 1 intact (not doubled)");
    TEST_ASSERT(out[2] == (char)0xE0, "lead byte 2 intact");
    TEST_ASSERT(out[3] == (char)0x40, "trail byte 2 intact");
}

TEST_CASE(overflow_returns_negative) {
    char out[4];
    int n;
    n = ArgvEscapeArg("hello world", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(-1, n, "too-small buffer -> -1");
}

/* ========================================================
 * PBT - spawn argv_echo.exe and roundtrip the argv vector
 * ======================================================== */

/* Locate argv_echo.exe next to this test executable. */
static int LocateArgvEcho(char *path, int pathSize)
{
    char self[MAX_PATH];
    DWORD len;
    int i, lastSlash;

    len = GetModuleFileNameA(NULL, self, sizeof(self));
    if (len == 0 || len >= sizeof(self)) {
        return 0;
    }
    lastSlash = -1;
    for (i = 0; self[i] != '\0'; i++) {
        if (self[i] == '\\' || self[i] == '/') {
            lastSlash = i;
        }
    }
    if (lastSlash < 0) {
        return 0;
    }
    self[lastSlash + 1] = '\0';
    if ((int)(lstrlen(self) + lstrlen("argv_echo.exe")) >= pathSize) {
        return 0;
    }
    lstrcpy(path, self);
    lstrcat(path, "argv_echo.exe");
    return 1;
}

/*
 * Spawn argv_echo.exe with the given command line, capture stdout, and parse:
 * first line = argc; then argc base64 lines decoded into out_args[].
 * Returns spawned argc, or -1 on failure.
 */
static int SpawnAndCapture(const char *cmdLine,
                           char out_args[][512], int maxArgs)
{
    char echoPath[MAX_PATH];
    static char fullCmd[40000];
    SECURITY_ATTRIBUTES sa;
    HANDLE rd, wr;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    static char buf[65536];
    int total;
    int argc, idx, lineStart, i;

    if (!LocateArgvEcho(echoPath, sizeof(echoPath))) {
        return -1;
    }
    /* cmdLine starts with the args; prepend the program path (quoted). */
    if ((int)(lstrlen(echoPath) + lstrlen(cmdLine) + 4) >= (int)sizeof(fullCmd)) {
        return -1;
    }
    fullCmd[0] = '"';
    fullCmd[1] = '\0';
    lstrcat(fullCmd, echoPath);
    lstrcat(fullCmd, "\" ");
    lstrcat(fullCmd, cmdLine);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, fullCmd, NULL, NULL, TRUE, 0,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);   /* parent must drop its write end so the read EOFs */

    total = 0;
    for (;;) {
        DWORD got;
        if (total >= (int)sizeof(buf) - 1) {
            break;
        }
        if (!ReadFile(rd, buf + total, (DWORD)(sizeof(buf) - 1 - total),
                      &got, NULL) || got == 0) {
            break;
        }
        total += (int)got;
    }
    buf[total] = '\0';
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* First line is argc. */
    argc = 0;
    i = 0;
    while (buf[i] >= '0' && buf[i] <= '9') {
        argc = argc * 10 + (buf[i] - '0');
        i++;
    }
    if (buf[i] == '\r') i++;
    if (buf[i] == '\n') i++;

    /* Remaining lines are base64(argv[k]); child argv[0] is the program path,
     * so the slot for our arg j is line (j+1), i.e. curArg-1 below. */
    lineStart = i;
    idx = i;
    {
        int curArg;
        curArg = 0;
        while (idx <= total && curArg < argc) {
            if (idx == total || buf[idx] == '\n') {
                char line[700];
                int llen;

                llen = idx - lineStart;
                if (llen > 0 && buf[lineStart + llen - 1] == '\r') {
                    llen--;
                }
                if (llen < 0) llen = 0;
                if (llen >= (int)sizeof(line)) llen = (int)sizeof(line) - 1;
                if (llen > 0) {
                    memcpy(line, buf + lineStart, (size_t)llen);
                }
                line[llen] = '\0';

                if (curArg >= 1 && (curArg - 1) < maxArgs) {
                    int slot;
                    slot = curArg - 1;
                    if (llen == 0) {
                        out_args[slot][0] = '\0';
                    } else {
                        unsigned char raw[512];
                        int decoded;
                        decoded = Base64Decode(line, raw, (int)sizeof(raw));
                        if (decoded < 0) decoded = 0;
                        if (decoded >= 512) decoded = 511;
                        if (decoded > 0) {
                            memcpy(out_args[slot], raw, (size_t)decoded);
                        }
                        out_args[slot][decoded] = '\0';
                    }
                }
                curArg++;
                lineStart = idx + 1;
            }
            idx++;
        }
    }

    return argc - 1;   /* exclude child argv[0] (the program path) */
}

PROP_TEST(argv_roundtrip_via_spawn) {
    char args[8][512];
    const char *argp[8];
    char joined[20000];
    char captured[8][512];
    int count, i, j, n, spawned;

    count = PROP_INT(1, 8);
    for (i = 0; i < count; i++) {
        int alen;
        alen = PROP_INT(0, 32);
        for (j = 0; j < alen; j++) {
            int pick;
            char c;
            pick = PROP_INT(0, 4);
            switch (pick) {
            case 0:  c = ' ';  break;
            case 1:  c = '\t'; break;
            case 2:  c = '\\'; break;
            case 3:  c = '"';  break;
            default: c = (char)PROP_INT(0x21, 0x7E); break;
            }
            args[i][j] = c;
        }
        args[i][alen] = '\0';
        argp[i] = args[i];
    }

    n = ArgvJoin(argp, count, joined, (int)sizeof(joined));
    PROP_CHECK(n >= 0);

    spawned = SpawnAndCapture(joined, captured, 8);
    PROP_CHECK(spawned == count);

    for (i = 0; i < count; i++) {
        PROP_CHECK(strcmp(captured[i], args[i]) == 0);
    }
}

int main(void)
{
    printf("Running argv tests...\n\n");

    RUN_TEST(escape_simple_verbatim);
    RUN_TEST(join_two_words);
    RUN_TEST(escape_embedded_space);
    RUN_TEST(escape_embedded_quote);
    RUN_TEST(escape_trailing_backslash_verbatim);
    RUN_TEST(escape_interior_backslash_verbatim);
    RUN_TEST(escape_trailing_backslash_before_quote);
    RUN_TEST(escape_empty_arg);
    RUN_TEST(join_with_empty_args);
    RUN_TEST(metachar_no_shell_quoted_only);
    RUN_TEST(metachar_shell_caret_escaped_outside_quotes);
    RUN_TEST(cmd_escape_inside_quotes_untouched);
    RUN_TEST(all_printable_ascii_roundtrips_internally);
    RUN_TEST(dbcs_lead_bytes_not_split);
    RUN_TEST(overflow_returns_negative);

    print_test_summary();

    printf("\nRunning argv property tests...\n\n");
    prop_seed(1);
    PROP_RUN(argv_roundtrip_via_spawn, 1000);

    if (g_tests_failed != 0) {
        return 1;
    }
    return prop_summary();
}
