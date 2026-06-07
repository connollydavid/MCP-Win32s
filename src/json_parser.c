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

int JsonEscape(const char *src, char *dst, int dst_size)
{
    return json_escape(src, dst, dst_size);
}

/*
 * copy_key - Null-terminate a key into a small buffer for comparison.
 */
static void copy_key(char *key_buf, int key_buf_size,
                     const char *key, int key_len)
{
    int copy_len;

    copy_len = key_len;
    if (copy_len >= key_buf_size) {
        copy_len = key_buf_size - 1;
    }
    memcpy(key_buf, key, copy_len);
    key_buf[copy_len] = '\0';
}

/*
 * assign_field - Copy a string value into the appropriate JsonCommand
 * field based on the key name. Unknown keys are silently ignored.
 */
static void assign_field(JsonCommand *cmd,
                         const char *key, int key_len,
                         const char *val, int val_len)
{
    char key_buf[64];

    copy_key(key_buf, (int)sizeof(key_buf), key, key_len);

    if (strcmp(key_buf, "cmd") == 0) {
        json_unescape(val, val_len, cmd->cmd, MCP_MAX_CMD);
    } else if (strcmp(key_buf, "id") == 0) {
        json_unescape(val, val_len, cmd->id, MCP_MAX_ID);
    } else if (strcmp(key_buf, "path") == 0) {
        json_unescape(val, val_len, cmd->path, MCP_MAX_PATH_LEN);
    } else if (strcmp(key_buf, "dest") == 0) {
        json_unescape(val, val_len, cmd->dest, MCP_MAX_PATH_LEN);
    } else if (strcmp(key_buf, "line") == 0) {
        json_unescape(val, val_len, cmd->line, MCP_MAX_LINE);
    } else if (strcmp(key_buf, "data") == 0) {
        json_unescape(val, val_len, cmd->data, MCP_MAX_DATA);
    } else if (strcmp(key_buf, "cwd") == 0) {
        json_unescape(val, val_len, cmd->cwd, MCP_MAX_PATH_LEN);
    } else if (strcmp(key_buf, "stdin_b64") == 0) {
        json_unescape(val, val_len, cmd->stdin_b64, MCP_MAX_STDIN_B64);
    } else if (strcmp(key_buf, "mem_token") == 0) {
        json_unescape(val, val_len, cmd->mem_token, MCP_MAX_MEM_TOKEN);
    } else if (strcmp(key_buf, "mem_addr") == 0) {
        json_unescape(val, val_len, cmd->mem_addr, MCP_MAX_MEM_NUM);
    } else if (strcmp(key_buf, "mem_len") == 0) {
        json_unescape(val, val_len, cmd->mem_len, MCP_MAX_MEM_NUM);
    }
    /* mem_addr/mem_len are STRINGS, not integers, so they live here and
     * never on the assign_int_field path - a 32-bit address overflows the
     * signed-int parser (spec: memory-ops.allium AddressIsWellFormed).
     * Unknown keys are silently ignored. */
}

/*
 * assign_int_field - Store a parsed integer value by key name.
 */
static void assign_int_field(JsonCommand *cmd,
                             const char *key, int key_len, int value)
{
    char key_buf[64];

    copy_key(key_buf, (int)sizeof(key_buf), key, key_len);

    if (strcmp(key_buf, "timeout_ms") == 0) {
        cmd->timeout_ms = value;
    } else if (strcmp(key_buf, "max_output") == 0) {
        cmd->max_output = value;
    } else if (strcmp(key_buf, "mem_cap_bytes") == 0) {
        cmd->mem_cap_bytes = value;
    } else if (strcmp(key_buf, "cpu_time_ms") == 0) {
        cmd->cpu_time_ms = value;
    } else if (strcmp(key_buf, "cols") == 0) {
        cmd->cols = value;
    } else if (strcmp(key_buf, "rows") == 0) {
        cmd->rows = value;
    }
    /* Unknown keys are silently ignored */
}

/*
 * assign_bool_field - Store a parsed boolean value by key name.
 */
static void assign_bool_field(JsonCommand *cmd,
                              const char *key, int key_len, int value)
{
    char key_buf[64];

    copy_key(key_buf, (int)sizeof(key_buf), key, key_len);

    if (strcmp(key_buf, "shell") == 0) {
        cmd->shell_flag = value;
    } else if (strcmp(key_buf, "unsafe") == 0) {
        cmd->unsafe_flag = value;
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

        /* Dispatch on value type: string, array, number, boolean, null */
        if (*p == '"') {
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

            assign_field(out, key_start, key_len, val_start, val_len);
        } else if (*p == '[') {
            /* Array values. "argv" is stored and must contain only
             * strings; arrays under UNKNOWN keys are scanned and
             * ignored whatever their scalar element kind - strings,
             * numbers, booleans, null (forward compatibility, spec:
             * json-parser.allium; weed 2026-06-06). Nested containers
             * stay rejected: the wire is single-level. */
            char key_buf[64];
            int is_argv;

            copy_key(key_buf, (int)sizeof(key_buf), key_start, key_len);
            is_argv = (strcmp(key_buf, "argv") == 0);
            p++;

            for (;;) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
                    p++;
                }
                if (*p == ']') {
                    p++;
                    break;
                }
                if (*p == ',') {
                    p++;
                    continue;
                }
                if (*p == '"') {
                    p++;
                    val_start = p;
                    while (*p != '\0') {
                        if (*p == '\\' && *(p + 1) != '\0') {
                            p += 2;
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

                    if (is_argv && out->argv_count < MCP_MAX_ARGV) {
                        json_unescape(val_start, val_len,
                                      out->argv[out->argv_count],
                                      MCP_MAX_ARG_LEN);
                        out->argv_count++;
                    }
                    /* Elements beyond MCP_MAX_ARGV are dropped
                     * silently; the dispatcher polices meaning. */
                } else if (is_argv) {
                    /* argv elements must be strings */
                    return 0;
                } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
                    if (*p == '-') {
                        p++;
                    }
                    if (*p < '0' || *p > '9') {
                        return 0;
                    }
                    while ((*p >= '0' && *p <= '9') || *p == '.') {
                        p++;
                    }
                } else if (strncmp(p, "true", 4) == 0) {
                    p += 4;
                } else if (strncmp(p, "false", 5) == 0) {
                    p += 5;
                } else if (strncmp(p, "null", 4) == 0) {
                    p += 4;
                } else {
                    return 0;
                }
            }
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            /* Integer value (no float support - FP is banned) */
            int sign;
            long acc;

            sign = 1;
            if (*p == '-') {
                sign = -1;
                p++;
            }
            if (*p < '0' || *p > '9') {
                return 0;
            }
            acc = 0;
            while (*p >= '0' && *p <= '9') {
                if (acc < 214748364L) {
                    acc = acc * 10 + (*p - '0');
                }
                p++;
            }
            if (*p == '.') {
                /* Fractional values are not part of the protocol */
                return 0;
            }
            assign_int_field(out, key_start, key_len, (int)(sign * acc));
        } else if (strncmp(p, "true", 4) == 0) {
            p += 4;
            assign_bool_field(out, key_start, key_len, 1);
        } else if (strncmp(p, "false", 5) == 0) {
            p += 5;
            assign_bool_field(out, key_start, key_len, 0);
        } else if (strncmp(p, "null", 4) == 0) {
            p += 4;
            /* null leaves the field zeroed */
        } else {
            return 0;
        }
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
    /* Static: 256KB would overflow/probe the stack; server is
     * single-threaded by hard constraint (Win32s). */
    static char escaped_value[MCP_MAX_RESPONSE];
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
