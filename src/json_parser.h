/*
 * json_parser.h - Hand-coded JSON parser for MCP-Win32s protocol
 *
 * Parses the simple single-level JSON object format used by the
 * MCP-Win32s command protocol. No external dependencies.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include "common.h"

/*
 * ParseJsonCommand - Parse a JSON command string into a JsonCommand struct.
 *
 * Input:  json - null-terminated JSON string, e.g.:
 *           {"cmd":"exec","id":"1","line":"dir"}
 * Output: out  - populated JsonCommand struct (zeroed first, then filled)
 *
 * Returns: 1 on success, 0 on parse error.
 * Missing fields are left zeroed. Unknown keys are silently ignored.
 */
int ParseJsonCommand(const char *json, JsonCommand *out);

/*
 * BuildJsonResponse - Build a JSON response string.
 *
 * Produces: {"id":"<id>","status":"<status>","<key>":"<escaped_value>"}\n
 *
 * The value is JSON-escaped (quotes, backslashes, control chars).
 *
 * Parameters:
 *   id        - request ID to echo back
 *   status    - "ok" or "error"
 *   key       - response data key (e.g. "output", "error", "data", "files")
 *   value     - response data value (will be JSON-escaped)
 *   json      - output buffer
 *   json_size - size of output buffer in bytes
 *
 * Returns: number of bytes written (excluding null terminator),
 *          or 0 if buffer too small.
 */
int BuildJsonResponse(const char *id, const char *status,
                      const char *key, const char *value,
                      char *json, int json_size);

/*
 * JsonEscape - Escape a string for inclusion as a JSON value.
 * Returns escaped length, or -1 if dst is too small. (Public wrapper
 * for builders of multi-key responses, e.g. the exec dispatcher.)
 */
int JsonEscape(const char *src, char *dst, int dst_size);

#endif /* JSON_PARSER_H */
