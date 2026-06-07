/*
 * ready.c - Extended per-connection ready message
 *
 * Spec: wire-contract.allium contract ReadyHandshake (the documented
 * keys are always present; values vary by host). The features object
 * mirrors g_features so the bridge can surface capability-aware UI.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include "feat.h"
#include "ready.h"
#include "toolchain_probe.h"
#include "mem_ops.h"
#include "encoding.h"

/*
 * append_str - Bounds-checked append; returns 1 on success.
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
 * append_int - Append a non-negative integer in decimal.
 */
static int append_int(char *dst, int dstSize, int *pos, int value)
{
    char buf[16];

    wsprintfA(buf, "%d", value);
    return append_str(dst, dstSize, pos, buf);
}

/*
 * append_bool_field - Append "key":true|false with optional comma.
 */
static int append_bool_field(char *dst, int dstSize, int *pos,
                             const char *key, int value, int comma)
{
    if (!append_str(dst, dstSize, pos, "\"")) return 0;
    if (!append_str(dst, dstSize, pos, key)) return 0;
    if (!append_str(dst, dstSize, pos, value ? "\":true" : "\":false")) return 0;
    if (comma && !append_str(dst, dstSize, pos, ",")) return 0;
    return 1;
}

int BuildReadyMessage(const char *transportName, const char *warning,
                      const ToolchainSet *toolchains,
                      char *json, int jsonSize)
{
    int pos;

    if (json == NULL || jsonSize < 1) {
        return 0;
    }
    json[0] = '\0';
    if (transportName == NULL) {
        transportName = "";
    }

    pos = 0;
    if (!append_str(json, jsonSize, &pos, "{\"status\":\"ready\",\"codepage\":")) goto fail;
    if (!append_int(json, jsonSize, &pos, (int)GetOEMCP())) goto fail;
    if (!append_str(json, jsonSize, &pos, ",\"version\":\"")) goto fail;
    if (!append_str(json, jsonSize, &pos, FeatVersionString())) goto fail;
    if (!append_str(json, jsonSize, &pos, "\",\"transport\":\"")) goto fail;
    if (!append_str(json, jsonSize, &pos, transportName)) goto fail;
    if (!append_str(json, jsonSize, &pos, "\",\"features\":{")) goto fail;

    if (!append_bool_field(json, jsonSize, &pos, "is_win32s", g_features.is_win32s, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "is_win9x", g_features.is_win9x, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "is_nt", g_features.is_nt, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "is_wow64", g_features.is_wow64, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "threads", g_features.has_threads, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "job_objects", g_features.has_create_job_object, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "ctrl_events", g_features.has_generate_ctrl_event, 1)) goto fail;
    if (!append_bool_field(json, jsonSize, &pos, "pty", g_features.has_create_pseudo_console, 1)) goto fail;

    if (!append_str(json, jsonSize, &pos, "\"binary_classify\":\"")) goto fail;
    if (!append_str(json, jsonSize, &pos,
                    g_features.has_get_binary_type ? "GetBinaryTypeA" : "manual")) goto fail;
    if (!append_str(json, jsonSize, &pos, "\",")) goto fail;

    if (!append_bool_field(json, jsonSize, &pos, "process_mitigation",
                           g_features.has_set_process_mitigation, 1)) goto fail;

    /* The memory-reach tier (spec: memory-ops.allium MemTier; wire-contract
     * ReadyShape features.mem). Always present; gates the bridge's five memory
     * tools (none -> all pruned). Derived from the OS family. */
    if (!append_str(json, jsonSize, &pos, "\"mem\":\"")) goto fail;
    if (!append_str(json, jsonSize, &pos, MemTierName(MemTierCurrent()))) goto fail;
    if (!append_str(json, jsonSize, &pos, "\",")) goto fail;

    /* The text-encoding provenance tag (spec: wire-contract.allium ReadyShape
     * features.encoding) - which tier the device transcoded the wire text from
     * (utf8_manifest|utf8_via_w|utf8_from_cp). INFORMATIONAL only: the device
     * emits valid UTF-8 on every tier, so the bridge never switches on it. */
    if (!append_str(json, jsonSize, &pos, "\"encoding\":\"")) goto fail;
    if (!append_str(json, jsonSize, &pos, EncProvenanceTag())) goto fail;
    if (!append_str(json, jsonSize, &pos, "\",")) goto fail;

    /* The detected build toolchains (spec: wire-contract.allium ReadyShape).
     * Always present inside features; empty when none was detected. */
    if (!ToolchainAppendJson(toolchains, json, jsonSize, &pos)) goto fail;

    if (!append_str(json, jsonSize, &pos, "}")) goto fail;

    if (warning != NULL && warning[0] != '\0') {
        if (!append_str(json, jsonSize, &pos, ",\"warning\":\"")) goto fail;
        if (!append_str(json, jsonSize, &pos, warning)) goto fail;
        if (!append_str(json, jsonSize, &pos, "\"")) goto fail;
    }

    if (!append_str(json, jsonSize, &pos, "}\n")) goto fail;
    return pos;

fail:
    json[0] = '\0';
    return 0;
}
