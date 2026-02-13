/*
 * common.h - Shared types and constants for MCP-Win32s
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef COMMON_H
#define COMMON_H

/* Field size constants */
#define MCP_MAX_CMD      32
#define MCP_MAX_ID       32
#define MCP_MAX_PATH_LEN 260    /* Same as Win32 MAX_PATH */
#define MCP_MAX_LINE     1024
#define MCP_MAX_DATA     65536  /* 64KB for base64 file data */
#define MCP_MAX_RESPONSE 131072 /* 128KB for JSON response buffer */

/*
 * JsonCommand - Parsed representation of an incoming JSON command.
 *
 * Protocol format:
 *   {"cmd":"exec","id":"123","line":"dir"}
 *   {"cmd":"read","id":"456","path":"C:\\test.txt"}
 *   {"cmd":"write","id":"789","path":"C:\\out.c","data":"<base64>"}
 *   {"cmd":"list","id":"012","path":"C:\\PROJECTS"}
 *   {"cmd":"delete","id":"345","path":"C:\\old.obj"}
 *
 * Fields not present in the JSON are left zeroed.
 */
typedef struct {
    char cmd[MCP_MAX_CMD];           /* Command: "exec","read","write","list","delete" */
    char id[MCP_MAX_ID];            /* Request correlation ID */
    char path[MCP_MAX_PATH_LEN];    /* File path (ANSI) */
    char line[MCP_MAX_LINE];        /* Command line for exec */
    char data[MCP_MAX_DATA];        /* Base64 encoded file data */
} JsonCommand;

#endif /* COMMON_H */
