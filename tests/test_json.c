/*
 * test_json.c - Unit tests for json_parser.c
 *
 * Tests JSON command parsing and response building for the
 * MCP-Win32s protocol. Covers happy paths, edge cases, and
 * error conditions.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "json_parser.h"

/* ========================================================
 * Parsing - Happy Path
 * ======================================================== */

TEST_CASE(parse_exec_command) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{\"cmd\":\"exec\",\"id\":\"1\",\"line\":\"dir\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("exec", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("1", cmd.id, "id field");
    TEST_ASSERT_STR_EQUAL("dir", cmd.line, "line field");
    TEST_ASSERT_STR_EQUAL("", cmd.path, "path should be empty");
    TEST_ASSERT_STR_EQUAL("", cmd.data, "data should be empty");
}

TEST_CASE(parse_read_command) {
    JsonCommand cmd;
    int result;
    /* JSON: {"cmd":"read","id":"2","path":"C:\\test.txt"} */
    result = ParseJsonCommand(
        "{\"cmd\":\"read\",\"id\":\"2\",\"path\":\"C:\\\\test.txt\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("read", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("2", cmd.id, "id field");
    TEST_ASSERT_STR_EQUAL("C:\\test.txt", cmd.path, "path with unescaped backslash");
}

TEST_CASE(parse_write_command) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"write\",\"id\":\"3\",\"path\":\"C:\\\\out.c\",\"data\":\"SGVsbG8=\"}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("write", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("3", cmd.id, "id field");
    TEST_ASSERT_STR_EQUAL("C:\\out.c", cmd.path, "path field");
    TEST_ASSERT_STR_EQUAL("SGVsbG8=", cmd.data, "data field (base64)");
}

TEST_CASE(parse_list_command) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"list\",\"id\":\"4\",\"path\":\"C:\\\\PROJECTS\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("list", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("C:\\PROJECTS", cmd.path, "path field");
}

TEST_CASE(parse_delete_command) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"delete\",\"id\":\"5\",\"path\":\"C:\\\\old.obj\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("delete", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("C:\\old.obj", cmd.path, "path field");
}

TEST_CASE(parse_all_fields) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"write\",\"id\":\"99\",\"path\":\"C:\\\\foo.c\","
        "\"line\":\"some line\",\"data\":\"AQID\"}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("write", cmd.cmd, "cmd");
    TEST_ASSERT_STR_EQUAL("99", cmd.id, "id");
    TEST_ASSERT_STR_EQUAL("C:\\foo.c", cmd.path, "path");
    TEST_ASSERT_STR_EQUAL("some line", cmd.line, "line");
    TEST_ASSERT_STR_EQUAL("AQID", cmd.data, "data");
}

/* ========================================================
 * Parsing - Edge Cases
 * ======================================================== */

TEST_CASE(parse_empty_string) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("", &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "empty string should fail");
}

TEST_CASE(parse_null_input) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(NULL, &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "NULL input should fail");
}

TEST_CASE(parse_null_output) {
    int result;
    result = ParseJsonCommand("{\"cmd\":\"exec\"}", NULL);
    TEST_ASSERT_INT_EQUAL(0, result, "NULL output should fail");
}

TEST_CASE(parse_empty_object) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "empty object should succeed");
    TEST_ASSERT_STR_EQUAL("", cmd.cmd, "cmd should be empty");
    TEST_ASSERT_STR_EQUAL("", cmd.id, "id should be empty");
    TEST_ASSERT_STR_EQUAL("", cmd.path, "path should be empty");
}

TEST_CASE(parse_missing_fields) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{\"cmd\":\"exec\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "partial object should succeed");
    TEST_ASSERT_STR_EQUAL("exec", cmd.cmd, "cmd present");
    TEST_ASSERT_STR_EQUAL("", cmd.id, "id zeroed");
    TEST_ASSERT_STR_EQUAL("", cmd.path, "path zeroed");
    TEST_ASSERT_STR_EQUAL("", cmd.line, "line zeroed");
}

TEST_CASE(parse_extra_whitespace) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "  { \"cmd\" : \"exec\" , \"id\" : \"7\" }  ", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "whitespace should be tolerated");
    TEST_ASSERT_STR_EQUAL("exec", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("7", cmd.id, "id field");
}

TEST_CASE(parse_escaped_quotes) {
    JsonCommand cmd;
    int result;
    /* JSON value: "say \"hello\"" -> unescaped: say "hello" */
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"line\":\"say \\\"hello\\\"\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("say \"hello\"", cmd.line, "escaped quotes in value");
}

TEST_CASE(parse_escaped_backslashes) {
    JsonCommand cmd;
    int result;
    /* JSON: C:\\Windows\\System32 -> unescaped: C:\Windows\System32 */
    result = ParseJsonCommand(
        "{\"path\":\"C:\\\\Windows\\\\System32\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("C:\\Windows\\System32", cmd.path,
        "double backslash to single");
}

TEST_CASE(parse_escaped_control_chars) {
    JsonCommand cmd;
    int result;
    /* JSON: "line1\nline2\ttab" -> unescaped: line1<newline>line2<tab>tab */
    result = ParseJsonCommand(
        "{\"line\":\"line1\\nline2\\ttab\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("line1\nline2\ttab", cmd.line,
        "escaped control characters");
}

TEST_CASE(parse_malformed_no_closing_brace) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{\"cmd\":\"exec\"", &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "missing closing brace should fail");
}

TEST_CASE(parse_malformed_no_quotes) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{cmd:exec}", &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "unquoted keys should fail");
}

TEST_CASE(parse_malformed_no_colon) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{\"cmd\"\"exec\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "missing colon should fail");
}

TEST_CASE(parse_malformed_no_value_quote) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{\"cmd\":exec}", &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "unquoted value should fail");
}

TEST_CASE(parse_unknown_keys_ignored) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"foo\":\"bar\",\"id\":\"10\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "unknown keys should not cause failure");
    TEST_ASSERT_STR_EQUAL("exec", cmd.cmd, "cmd field");
    TEST_ASSERT_STR_EQUAL("10", cmd.id, "id field");
}

TEST_CASE(parse_value_truncation) {
    JsonCommand cmd;
    int result;
    char long_cmd[128];
    /* Create a value longer than MCP_MAX_CMD (32) */
    memset(long_cmd, 'A', 64);
    long_cmd[64] = '\0';

    {
        char json[256];
        sprintf(json, "{\"cmd\":\"%s\"}", long_cmd);
        result = ParseJsonCommand(json, &cmd);
        TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed with truncation");
        /* cmd field is 32 bytes, so value should be truncated to 31 chars + null */
        TEST_ASSERT(strlen(cmd.cmd) == MCP_MAX_CMD - 1,
            "value should be truncated to field size");
    }
}

TEST_CASE(parse_just_whitespace) {
    JsonCommand cmd;
    int result;
    result = ParseJsonCommand("   \t\n  ", &cmd);
    TEST_ASSERT_INT_EQUAL(0, result, "whitespace-only should fail");
}

/* ========================================================
 * Response Building
 * ======================================================== */

TEST_CASE(build_ok_response) {
    char buf[512];
    int len;
    len = BuildJsonResponse("1", "ok", "output", "hello", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"1\",\"status\":\"ok\",\"output\":\"hello\"}\n",
        buf, "ok response format");
}

TEST_CASE(build_error_response) {
    char buf[512];
    int len;
    len = BuildJsonResponse("2", "error", "error", "not found", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"2\",\"status\":\"error\",\"error\":\"not found\"}\n",
        buf, "error response format");
}

TEST_CASE(build_response_escapes_quotes) {
    char buf[512];
    int len;
    len = BuildJsonResponse("3", "ok", "output", "say \"hi\"", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"3\",\"status\":\"ok\",\"output\":\"say \\\"hi\\\"\"}\n",
        buf, "quotes should be escaped");
}

TEST_CASE(build_response_escapes_backslash) {
    char buf[512];
    int len;
    len = BuildJsonResponse("4", "ok", "output", "C:\\path", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"4\",\"status\":\"ok\",\"output\":\"C:\\\\path\"}\n",
        buf, "backslash should be escaped");
}

TEST_CASE(build_response_escapes_newlines) {
    char buf[512];
    int len;
    len = BuildJsonResponse("5", "ok", "output", "line1\nline2", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"5\",\"status\":\"ok\",\"output\":\"line1\\nline2\"}\n",
        buf, "newlines should be escaped");
}

TEST_CASE(build_response_escapes_tabs) {
    char buf[512];
    int len;
    len = BuildJsonResponse("6", "ok", "output", "col1\tcol2", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"6\",\"status\":\"ok\",\"output\":\"col1\\tcol2\"}\n",
        buf, "tabs should be escaped");
}

TEST_CASE(build_response_buffer_limit) {
    char buf[16]; /* Intentionally small */
    int len;
    len = BuildJsonResponse("1", "ok", "output", "hello world", buf, sizeof(buf));
    TEST_ASSERT_INT_EQUAL(0, len, "should return 0 when buffer too small");
    TEST_ASSERT_STR_EQUAL("", buf, "buffer should be empty on overflow");
}

TEST_CASE(build_response_empty_value) {
    char buf[512];
    int len;
    len = BuildJsonResponse("7", "ok", "output", "", buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"7\",\"status\":\"ok\",\"output\":\"\"}\n",
        buf, "empty value");
}

TEST_CASE(build_response_null_value) {
    char buf[512];
    int len;
    len = BuildJsonResponse("8", "ok", "output", NULL, buf, sizeof(buf));
    TEST_ASSERT(len > 0, "should return positive length");
    TEST_ASSERT_STR_EQUAL(
        "{\"id\":\"8\",\"status\":\"ok\",\"output\":\"\"}\n",
        buf, "NULL value treated as empty");
}

/* ========================================================
 * Parsing - Phase 4 exec fields
 * Obligation: entity-fields.Command (mcp-protocol.allium) - argv
 * array, cwd, shell/unsafe booleans, timeout_ms/max_output ints,
 * stdin_b64. See tests/OBLIGATIONS-PHASE4.md.
 * ======================================================== */

TEST_CASE(parse_exec_argv_array) {
    static JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"id\":\"e1\",\"argv\":[\"cl\",\"/c\",\"a b.c\"]}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_INT_EQUAL(3, cmd.argv_count, "argv count");
    TEST_ASSERT_STR_EQUAL("cl", cmd.argv[0], "argv[0]");
    TEST_ASSERT_STR_EQUAL("/c", cmd.argv[1], "argv[1]");
    TEST_ASSERT_STR_EQUAL("a b.c", cmd.argv[2], "argv[2] with space");
}

TEST_CASE(parse_exec_argv_escapes) {
    static JsonCommand cmd;
    int result;
    /* argv element with escaped quote and backslash: he said "hi" + C:\ */
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"id\":\"e2\",\"argv\":[\"say \\\"hi\\\"\",\"C:\\\\\"]}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_INT_EQUAL(2, cmd.argv_count, "argv count");
    TEST_ASSERT_STR_EQUAL("say \"hi\"", cmd.argv[0], "embedded quotes");
    TEST_ASSERT_STR_EQUAL("C:\\", cmd.argv[1], "trailing backslash");
}

TEST_CASE(parse_exec_numbers_and_bools) {
    static JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"id\":\"e3\",\"timeout_ms\":30000,"
        "\"max_output\":4096,\"shell\":true,\"unsafe\":false,"
        "\"mem_cap_bytes\":8388608,\"cpu_time_ms\":1000,"
        "\"cols\":80,\"rows\":25}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_INT_EQUAL(30000, cmd.timeout_ms, "timeout_ms");
    TEST_ASSERT_INT_EQUAL(4096, cmd.max_output, "max_output");
    TEST_ASSERT_INT_EQUAL(1, cmd.shell_flag, "shell true");
    TEST_ASSERT_INT_EQUAL(0, cmd.unsafe_flag, "unsafe false");
    TEST_ASSERT_INT_EQUAL(8388608, cmd.mem_cap_bytes, "mem_cap_bytes");
    TEST_ASSERT_INT_EQUAL(1000, cmd.cpu_time_ms, "cpu_time_ms");
    TEST_ASSERT_INT_EQUAL(80, cmd.cols, "cols");
    TEST_ASSERT_INT_EQUAL(25, cmd.rows, "rows");
}

TEST_CASE(parse_exec_cwd_and_stdin) {
    static JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"id\":\"e4\",\"cwd\":\"C:\\\\PROJECTS\","
        "\"stdin_b64\":\"Zm9vCg==\"}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_STR_EQUAL("C:\\PROJECTS", cmd.cwd, "cwd");
    TEST_ASSERT_STR_EQUAL("Zm9vCg==", cmd.stdin_b64, "stdin_b64");
}

TEST_CASE(parse_exec_defaults_zeroed) {
    static JsonCommand cmd;
    int result;
    result = ParseJsonCommand("{\"cmd\":\"exec\",\"id\":\"e5\"}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_INT_EQUAL(0, cmd.argv_count, "argv absent");
    TEST_ASSERT_INT_EQUAL(0, cmd.timeout_ms, "timeout_ms 0 = server default");
    TEST_ASSERT_INT_EQUAL(0, cmd.shell_flag, "shell defaults false");
    TEST_ASSERT_INT_EQUAL(0, cmd.unsafe_flag, "unsafe defaults false");
    TEST_ASSERT_INT_EQUAL(0, cmd.max_output, "max_output 0 = cap");
}

TEST_CASE(parse_exec_negative_number) {
    static JsonCommand cmd;
    int result;
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"id\":\"e6\",\"timeout_ms\":-5}", &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_INT_EQUAL(-5, cmd.timeout_ms, "negative parsed");
}

TEST_CASE(parse_exec_unknown_array_ignored) {
    static JsonCommand cmd;
    int result;
    /* Arrays under unknown keys are scanned and dropped */
    result = ParseJsonCommand(
        "{\"cmd\":\"exec\",\"id\":\"e7\",\"future\":[\"x\",\"y\"],"
        "\"argv\":[\"dir\"]}",
        &cmd);
    TEST_ASSERT_INT_EQUAL(1, result, "parse should succeed");
    TEST_ASSERT_INT_EQUAL(1, cmd.argv_count, "only argv stored");
    TEST_ASSERT_STR_EQUAL("dir", cmd.argv[0], "argv[0]");
}

TEST_CASE(parse_exec_unknown_array_scalars_ignored) {
    /* Obligation: json-parser.allium rule LineParses - unknown keys
     * are ignored whatever their scalar value kind, including arrays
     * of numbers/booleans/null (weed 2026-06-06 pinning test). argv
     * itself stays strings-only. */
    static JsonCommand cmd;
    TEST_ASSERT_INT_EQUAL(1,
        ParseJsonCommand("{\"cmd\":\"exec\",\"id\":\"u1\","
                         "\"future\":[1,2,-3]}", &cmd),
        "unknown numeric array tolerated");
    TEST_ASSERT_STR_EQUAL("u1", cmd.id, "fields still parsed");
    TEST_ASSERT_INT_EQUAL(1,
        ParseJsonCommand("{\"future\":[true,false,null,\"x\",7]}", &cmd),
        "unknown mixed-scalar array tolerated");
    /* argv remains strings-only */
    TEST_ASSERT_INT_EQUAL(0,
        ParseJsonCommand("{\"argv\":[1,2]}", &cmd),
        "non-string argv element still rejected");
    /* nested containers stay rejected (single-level wire) */
    TEST_ASSERT_INT_EQUAL(0,
        ParseJsonCommand("{\"future\":[[1]]}", &cmd),
        "nested array rejected");
}

TEST_CASE(parse_exec_malformed_values) {
    static JsonCommand cmd;
    /* Fractional number: rejected (no FP in the protocol) */
    TEST_ASSERT_INT_EQUAL(0,
        ParseJsonCommand("{\"timeout_ms\":1.5}", &cmd),
        "fraction rejected");
    /* Bare word value: rejected */
    TEST_ASSERT_INT_EQUAL(0,
        ParseJsonCommand("{\"shell\":maybe}", &cmd),
        "bare word rejected");
    /* Unterminated array: rejected */
    TEST_ASSERT_INT_EQUAL(0,
        ParseJsonCommand("{\"argv\":[\"a\"", &cmd),
        "unterminated array rejected");
    /* Non-string array element: rejected */
    TEST_ASSERT_INT_EQUAL(0,
        ParseJsonCommand("{\"argv\":[1,2]}", &cmd),
        "non-string element rejected");
    /* null tolerated, leaves field zeroed */
    TEST_ASSERT_INT_EQUAL(1,
        ParseJsonCommand("{\"cmd\":\"exec\",\"cwd\":null}", &cmd),
        "null value tolerated");
    TEST_ASSERT_STR_EQUAL("", cmd.cwd, "null leaves cwd empty");
}

/* ========================================================
 * Main - Run all tests
 * ======================================================== */

int main(void)
{
    printf("JSON Parser Tests\n");
    printf("========================================\n");

    printf("\nParsing - Happy Path:\n");
    RUN_TEST(parse_exec_command);
    RUN_TEST(parse_read_command);
    RUN_TEST(parse_write_command);
    RUN_TEST(parse_list_command);
    RUN_TEST(parse_delete_command);
    RUN_TEST(parse_all_fields);

    printf("\nParsing - Edge Cases:\n");
    RUN_TEST(parse_empty_string);
    RUN_TEST(parse_null_input);
    RUN_TEST(parse_null_output);
    RUN_TEST(parse_empty_object);
    RUN_TEST(parse_missing_fields);
    RUN_TEST(parse_extra_whitespace);
    RUN_TEST(parse_escaped_quotes);
    RUN_TEST(parse_escaped_backslashes);
    RUN_TEST(parse_escaped_control_chars);
    RUN_TEST(parse_malformed_no_closing_brace);
    RUN_TEST(parse_malformed_no_quotes);
    RUN_TEST(parse_malformed_no_colon);
    RUN_TEST(parse_malformed_no_value_quote);
    RUN_TEST(parse_unknown_keys_ignored);
    RUN_TEST(parse_value_truncation);
    RUN_TEST(parse_just_whitespace);

    printf("\nParsing - Phase 4 exec fields:\n");
    RUN_TEST(parse_exec_argv_array);
    RUN_TEST(parse_exec_argv_escapes);
    RUN_TEST(parse_exec_numbers_and_bools);
    RUN_TEST(parse_exec_cwd_and_stdin);
    RUN_TEST(parse_exec_defaults_zeroed);
    RUN_TEST(parse_exec_negative_number);
    RUN_TEST(parse_exec_unknown_array_ignored);
    RUN_TEST(parse_exec_unknown_array_scalars_ignored);
    RUN_TEST(parse_exec_malformed_values);

    printf("\nResponse Building:\n");
    RUN_TEST(build_ok_response);
    RUN_TEST(build_error_response);
    RUN_TEST(build_response_escapes_quotes);
    RUN_TEST(build_response_escapes_backslash);
    RUN_TEST(build_response_escapes_newlines);
    RUN_TEST(build_response_escapes_tabs);
    RUN_TEST(build_response_buffer_limit);
    RUN_TEST(build_response_empty_value);
    RUN_TEST(build_response_null_value);

    print_test_summary();
    return g_tests_failed;
}
