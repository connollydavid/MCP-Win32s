/*
 * json_parser.c - Hand-coded JSON parser for MCP-Win32s protocol
 *
 * Parses single-level JSON objects: {"key":"value","key":"value",...}
 * No nested objects or arrays. ~200 lines of C89.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "json_parser.h"

/*
 * json_unescape - Unescape a JSON string value in-place.
 *
 * Converts: \" -> "   \\ -> \   \/ -> /   \n -> newline
 *           \t -> tab  \r -> CR  \b -> BS  \f -> FF
 *
 * src points to the raw characters between quotes (may contain escapes).
 * src_len is the number of raw characters.
 * dst receives the unescaped result (may alias src if dst <= src).
 * dst_size is the max bytes dst can hold (including null terminator).
 *
 * Returns: length of unescaped string (excluding null terminator).
 */
static int json_unescape(const char *src, int src_len, char *dst, int dst_size)
{
    int i;
    int out;
    char ch;

    out = 0;
    for (i = 0; i < src_len && out < dst_size - 1; i++) {
        ch = src[i];
        if (ch == '\\' && i + 1 < src_len) {
            i++;
            switch (src[i]) {
            case '"':  dst[out++] = '"';  break;
            case '\\': dst[out++] = '\\'; break;
            case '/':  dst[out++] = '/';  break;
            case 'n':  dst[out++] = '\n'; break;
            case 't':  dst[out++] = '\t'; break;
            case 'r':  dst[out++] = '\r'; break;
            case 'b':  dst[out++] = '\b'; break;
            case 'f':  dst[out++] = '\f'; break;
            default:
                /* Unknown escape - keep as-is */
                dst[out++] = '\\';
                if (out < dst_size - 1) {
                    dst[out++] = src[i];
                }
                break;
            }
        } else {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
    return out;
}

/*
 * json_escape - Escape a string for inclusion in a JSON value.
 *
 * Converts: " -> \"   \ -> \\   newline -> \n   tab -> \t
 *           CR -> \r  BS -> \b  FF -> \f
 *
 * Returns: length of escaped string (excluding null terminator),
 *          or -1 if dst buffer is too small.
 */
static int json_escape(const char *src, char *dst, int dst_size)
{
    int out;
    const char *p;

    out = 0;
    for (p = src; *p != '\0'; p++) {
        switch (*p) {
        case '"':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = '"';
            break;
        case '\\':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = '\\';
            break;
        case '\n':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = 'n';
            break;
        case '\t':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = 't';
            break;
        case '\r':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = 'r';
            break;
        case '\b':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = 'b';
            break;
        case '\f':
            if (out + 2 > dst_size - 1) return -1;
            dst[out++] = '\\';
            dst[out++] = 'f';
            break;
        default:
            if (out + 1 > dst_size - 1) return -1;
            dst[out++] = *p;
            break;
        }
    }
    dst[out] = '\0';
    return out;
}

/*
 * assign_field - Copy a value into the appropriate JsonCommand field
 * based on the key name. Unknown keys are silently ignored.
 */
static void assign_field(JsonCommand *cmd,
                         const char *key, int key_len,
                         const char *val, int val_len)
{
    char key_buf[64];
    int copy_len;

    /* Null-terminate the key for comparison */
    copy_len = key_len;
    if (copy_len >= (int)sizeof(key_buf)) {
        copy_len = (int)sizeof(key_buf) - 1;
    }
    memcpy(key_buf, key, copy_len);
    key_buf[copy_len] = '\0';

    if (strcmp(key_buf, "cmd") == 0) {
        json_unescape(val, val_len, cmd->cmd, MCP_MAX_CMD);
    } else if (strcmp(key_buf, "id") == 0) {
        json_unescape(val, val_len, cmd->id, MCP_MAX_ID);
    } else if (strcmp(key_buf, "path") == 0) {
        json_unescape(val, val_len, cmd->path, MCP_MAX_PATH_LEN);
    } else if (strcmp(key_buf, "line") == 0) {
        json_unescape(val, val_len, cmd->line, MCP_MAX_LINE);
    } else if (strcmp(key_buf, "data") == 0) {
        json_unescape(val, val_len, cmd->data, MCP_MAX_DATA);
    }
    /* Unknown keys are silently ignored */
}

int ParseJsonCommand(const char *json, JsonCommand *out)
{
    const char *p;

    if (json == NULL || out == NULL) {
        return 0;
    }

    memset(out, 0, sizeof(JsonCommand));

    /* Skip leading whitespace */
    p = json;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }

    /* Expect opening brace */
    if (*p != '{') {
        return 0;
    }
    p++;

    /* Parse key-value pairs */
    while (*p != '\0') {
        const char *key_start;
        const char *val_start;
        int key_len;
        int val_len;

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }

        /* Check for end of object */
        if (*p == '}') {
            return 1;
        }

        /* Skip comma between pairs */
        if (*p == ',') {
            p++;
            continue;
        }

        /* Expect opening quote for key */
        if (*p != '"') {
            return 0;
        }
        p++;
        key_start = p;

        /* Find closing quote for key (keys should not contain escapes) */
        while (*p != '\0' && *p != '"') {
            p++;
        }
        if (*p != '"') {
            return 0;
        }
        key_len = (int)(p - key_start);
        p++;

        /* Skip whitespace and colon */
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != ':') {
            return 0;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        /* Expect opening quote for value */
        if (*p != '"') {
            return 0;
        }
        p++;
        val_start = p;

        /* Find closing quote for value, handling escapes */
        while (*p != '\0') {
            if (*p == '\\' && *(p + 1) != '\0') {
                p += 2; /* Skip escaped character */
            } else if (*p == '"') {
                break;
            } else {
                p++;
            }
        }
        if (*p != '"') {
            return 0;
        }
        val_len = (int)(p - val_start);
        p++;

        /* Assign to the appropriate field */
        assign_field(out, key_start, key_len, val_start, val_len);
    }

    /* Reached end of string without closing brace */
    return 0;
}

/*
 * safe_append - Append src to dst at position *pos, respecting dst_size.
 * Returns 1 on success, 0 if buffer would overflow (dst unchanged).
 */
static int safe_append(char *dst, int dst_size, int *pos, const char *src)
{
    int len;
    int i;

    len = 0;
    while (src[len] != '\0') {
        len++;
    }

    if (*pos + len >= dst_size) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        dst[*pos + i] = src[i];
    }
    *pos += len;
    dst[*pos] = '\0';
    return 1;
}

int BuildJsonResponse(const char *id, const char *status,
                      const char *key, const char *value,
                      char *json, int json_size)
{
    char escaped_value[MCP_MAX_RESPONSE];
    int pos;

    if (json == NULL || json_size < 1) {
        return 0;
    }

    json[0] = '\0';

    /* Escape the value for JSON */
    if (value == NULL) {
        escaped_value[0] = '\0';
    } else if (json_escape(value, escaped_value, (int)sizeof(escaped_value)) < 0) {
        escaped_value[0] = '\0';
    }

    /* Ensure inputs are not NULL */
    if (id == NULL) id = "";
    if (status == NULL) status = "";
    if (key == NULL) key = "";

    /*
     * Build: {"id":"<id>","status":"<status>","<key>":"<escaped_value>"}\n
     * Using safe_append for bounds-checked concatenation.
     */
    pos = 0;
    if (!safe_append(json, json_size, &pos, "{\"id\":\"") ||
        !safe_append(json, json_size, &pos, id) ||
        !safe_append(json, json_size, &pos, "\",\"status\":\"") ||
        !safe_append(json, json_size, &pos, status) ||
        !safe_append(json, json_size, &pos, "\",\"") ||
        !safe_append(json, json_size, &pos, key) ||
        !safe_append(json, json_size, &pos, "\":\"") ||
        !safe_append(json, json_size, &pos, escaped_value) ||
        !safe_append(json, json_size, &pos, "\"}\n")) {
        json[0] = '\0';
        return 0;
    }

    return pos;
}
