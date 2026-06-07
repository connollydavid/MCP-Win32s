/*
 * toolchain_probe.c - Startup detection of installed build toolchains
 *
 * Spec: toolchains.allium (DetectedToolchain, the ToolchainDetected rule) and
 * wire-contract.allium ReadyShape. See toolchain_probe.h for the contract.
 *
 * The probe is pure string logic plus one thin wrapper over the existing exec
 * core: for each KNOWN built-in build command present in the catalog, run it
 * bare, read its version banner, and (if recognised) record a DetectedToolchain.
 * The banner formats are documented in docs/build-toolchain-flags.md.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include "toolchain_probe.h"
#include "exec_ops.h"
#include "binfmt.h"

/*
 * KnownProbe - one recognised built-in build command. anchor is the substring
 * that immediately precedes the version token in the banner (e.g. "Version ").
 */
typedef struct {
    const char *command;
    const char *vendor;
    const char *anchor;
} KnownProbe;

/*
 * The recognised built-in probes. MSVC cl and the Open Watcom compiler/driver
 * both print "... Version <token> ..." (docs/build-toolchain-flags.md):
 *   Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 12.00.8804 for 80x86
 *   Open Watcom C/C++ x86 32-bit Compile and Link Utility Version 2.0 ...
 */
static const KnownProbe g_known[] = {
    { "cl",      "Microsoft",   "Version " },
    { "wcc386",  "Open Watcom", "Version " },
    { "wcl386",  "Open Watcom", "Version " }
};

#define KNOWN_COUNT ((int)(sizeof(g_known) / sizeof(g_known[0])))

/*
 * Probe-run sizing. The banner buffers are generous; only the leading line
 * with the version matters. The timeout is short - a toolchain that does not
 * answer quickly is simply not detected.
 */
#define PROBE_TIMEOUT_MS   5000
#define PROBE_BANNER_MAX   4096

/*
 * find_substr - Return a pointer to the first occurrence of needle in hay, or
 * NULL. Pure ANSI scan (lstrlenA-bounded); no OS calls beyond string length.
 */
static const char *find_substr(const char *hay, const char *needle)
{
    int hayLen;
    int needleLen;
    int i;
    int j;

    hayLen = lstrlenA(hay);
    needleLen = lstrlenA(needle);
    if (needleLen == 0) {
        return hay;
    }
    if (needleLen > hayLen) {
        return NULL;
    }
    for (i = 0; i + needleLen <= hayLen; i++) {
        for (j = 0; j < needleLen; j++) {
            if (hay[i + j] != needle[j]) {
                break;
            }
        }
        if (j == needleLen) {
            return hay + i;
        }
    }
    return NULL;
}

/*
 * is_space_byte - 1 for ASCII whitespace, 0 otherwise.
 */
static int is_space_byte(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '\f' || c == '\v');
}

/*
 * copy_bounded - Copy at most dstSize-1 bytes from src, NUL-terminating.
 */
static void copy_bounded(char *dst, int dstSize, const char *src)
{
    int i;

    for (i = 0; i < dstSize - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int ToolchainMatchBanner(const char *command, const char *banner,
                         DetectedToolchain *out)
{
    int k;

    if (command == NULL || banner == NULL || out == NULL) {
        return 0;
    }

    for (k = 0; k < KNOWN_COUNT; k++) {
        const char *anchorPos;
        const char *verStart;
        int verLen;

        if (lstrcmpiA(command, g_known[k].command) != 0) {
            continue;
        }

        anchorPos = find_substr(banner, g_known[k].anchor);
        if (anchorPos == NULL) {
            continue;
        }

        /* The version token follows the anchor; skip any leading spaces. */
        verStart = anchorPos + lstrlenA(g_known[k].anchor);
        while (*verStart != '\0' && is_space_byte(*verStart)) {
            verStart++;
        }
        if (*verStart == '\0') {
            continue;
        }

        /* The token runs to the next whitespace. */
        verLen = 0;
        while (verStart[verLen] != '\0' && !is_space_byte(verStart[verLen])) {
            verLen++;
        }
        if (verLen == 0) {
            continue;
        }

        copy_bounded(out->vendor, TOOLCHAIN_VENDOR_MAX, g_known[k].vendor);
        copy_bounded(out->command, TOOLCHAIN_COMMAND_MAX, command);
        {
            int n;
            if (verLen > TOOLCHAIN_VERSION_MAX - 1) {
                verLen = TOOLCHAIN_VERSION_MAX - 1;
            }
            for (n = 0; n < verLen; n++) {
                out->version[n] = verStart[n];
            }
            out->version[verLen] = '\0';
        }
        return 1;
    }

    return 0;
}

int ToolchainProbe(const Catalog *cat, ToolchainSet *out)
{
    int k;

    if (out == NULL) {
        return 0;
    }
    out->count = 0;
    if (cat == NULL) {
        return 0;
    }

    for (k = 0; k < KNOWN_COUNT && out->count < TOOLCHAIN_MAX; k++) {
        unsigned char stdoutBuf[PROBE_BANNER_MAX];
        unsigned char stderrBuf[PROBE_BANNER_MAX];
        char banner[PROBE_BANNER_MAX * 2];
        char errMsg[128];
        ExecResult res;
        DetectedToolchain det;
        int ok;
        int pos;
        int i;

        if (CatalogLookup(cat, g_known[k].command) == NULL) {
            continue;
        }

        /* Run the command bare; its banner appears on stdout and/or stderr.
         * ExecOpRun already wraps the spawn in SetErrorMode (hard-error
         * dialogs suppressed), so a misbehaving tool cannot block us. */
        memset(&res, 0, sizeof(res));
        ok = ExecOpRun(g_known[k].command, NULL, PROBE_TIMEOUT_MS, 1,
                       NULL, 0,
                       stdoutBuf, (int)sizeof(stdoutBuf),
                       stderrBuf, (int)sizeof(stderrBuf),
                       0, 0, (int)BIN_PE32, &res, errMsg, (int)sizeof(errMsg));
        if (!ok) {
            continue;
        }

        /* Merge stdout then stderr into one scannable, NUL-terminated buffer. */
        pos = 0;
        for (i = 0; i < res.stdout_len && pos < (int)sizeof(banner) - 1; i++) {
            banner[pos++] = (char)stdoutBuf[i];
        }
        for (i = 0; i < res.stderr_len && pos < (int)sizeof(banner) - 1; i++) {
            banner[pos++] = (char)stderrBuf[i];
        }
        banner[pos] = '\0';

        if (ToolchainMatchBanner(g_known[k].command, banner, &det)) {
            out->items[out->count] = det;
            out->count++;
        }
    }

    return out->count;
}

/*
 * append_str - Bounds-checked append; advances *pos, returns 1 on success.
 * Mirrors ready.c's append_str helper.
 */
static int append_str(char *dst, int dstSize, int *pos, const char *src)
{
    int len;
    int i;

    len = lstrlenA(src);
    if (*pos + len >= dstSize) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        dst[*pos + i] = src[i];
    }
    *pos += len;
    dst[*pos] = '\0';
    return 1;
}

/*
 * append_escaped - Append a JSON string body, escaping " and \ (and the
 * control bytes that would otherwise break the JSON). Bounds-checked.
 */
static int append_escaped(char *dst, int dstSize, int *pos, const char *src)
{
    int i;
    int len;

    len = lstrlenA(src);
    for (i = 0; i < len; i++) {
        char c;
        const char *esc;

        c = src[i];
        esc = NULL;
        if (c == '"') {
            esc = "\\\"";
        } else if (c == '\\') {
            esc = "\\\\";
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c == '\t') {
            esc = "\\t";
        }

        if (esc != NULL) {
            if (!append_str(dst, dstSize, pos, esc)) {
                return 0;
            }
        } else {
            if (*pos + 1 >= dstSize) {
                return 0;
            }
            dst[*pos] = c;
            *pos += 1;
            dst[*pos] = '\0';
        }
    }
    return 1;
}

int ToolchainAppendJson(const ToolchainSet *set, char *json, int jsonSize,
                        int *pos)
{
    int i;
    int count;

    if (json == NULL || pos == NULL) {
        return 0;
    }

    if (!append_str(json, jsonSize, pos, "\"toolchains\":[")) return 0;

    count = (set != NULL) ? set->count : 0;
    for (i = 0; i < count; i++) {
        if (i > 0) {
            if (!append_str(json, jsonSize, pos, ",")) return 0;
        }
        if (!append_str(json, jsonSize, pos, "{\"vendor\":\"")) return 0;
        if (!append_escaped(json, jsonSize, pos, set->items[i].vendor)) return 0;
        if (!append_str(json, jsonSize, pos, "\",\"command\":\"")) return 0;
        if (!append_escaped(json, jsonSize, pos, set->items[i].command)) return 0;
        if (!append_str(json, jsonSize, pos, "\",\"version\":\"")) return 0;
        if (!append_escaped(json, jsonSize, pos, set->items[i].version)) return 0;
        if (!append_str(json, jsonSize, pos, "\"}")) return 0;
    }

    if (!append_str(json, jsonSize, pos, "]")) return 0;
    return 1;
}
