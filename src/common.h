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
#define MCP_MAX_RESPONSE 262144 /* 256KB: two base64 64KB streams + envelope */
#define MCP_MAX_ARGV     64     /* Max exec argv elements */
#define MCP_MAX_ARG_LEN  512    /* Max bytes per exec argv element */
#define MCP_MAX_STDIN_B64 8192  /* Base64 of the 4096-byte stdin cap + slack */
#define MCP_MAX_MEM_TOKEN 40    /* spawn-retain token "m<seq>-<rand>" + slack */
#define MCP_MAX_MEM_NUM   24    /* address/length wire string (hex or decimal) */

/*
 * JsonCommand - Parsed representation of an incoming JSON command.
 *
 * Protocol format:
 *   {"cmd":"exec","id":"123","argv":["cl","/c","x.c"],"timeout_ms":30000}
 *   {"cmd":"read","id":"456","path":"C:\\test.txt"}
 *   {"cmd":"write","id":"789","path":"C:\\out.c","data":"<base64>"}
 *   {"cmd":"list","id":"012","path":"C:\\PROJECTS"}
 *   {"cmd":"delete","id":"345","path":"C:\\old.obj"}
 *   {"cmd":"copy","id":"678","path":"C:\\a.txt","dest":"C:\\b.txt"}
 *   {"cmd":"move","id":"901","path":"C:\\a.txt","dest":"C:\\b.txt"}
 *   {"cmd":"mkdir","id":"234","path":"C:\\NEWDIR"}
 *   {"cmd":"rmdir","id":"567","path":"C:\\OLDDIR"}
 *   {"cmd":"spawnRetain","id":"a","argv":["cl"],"cwd":"C:\\"}
 *   {"cmd":"peek","id":"b","mem_token":"m1-742","mem_addr":"0x401000","mem_len":"64"}
 *   {"cmd":"poke","id":"c","mem_token":"m1-742","mem_addr":"0x401000","data":"<base64>"}
 *   {"cmd":"terminate","id":"d","mem_token":"m1-742"}
 *   {"cmd":"release","id":"e","mem_token":"m1-742"}
 *
 * Fields not present in the JSON are left zeroed. The exec-only fields
 * (argv..rows) are parsed for every command but only consumed by the
 * exec/ptyExec path (spec: mcp-protocol.allium entity Command); dest is
 * consumed only by the copy/move path (the source rides in path). The
 * mem_* fields (spec: memory-ops.allium) carry the spawnRetain/peek/poke/
 * terminate/release wire args; mem_addr/mem_len are STRINGS (a 32-bit
 * address overflows the signed-int JSON parser), parsed via MemParseU32.
 */
typedef struct {
    char cmd[MCP_MAX_CMD];           /* Command: "exec","read","write","list","delete" */
    char id[MCP_MAX_ID];            /* Request correlation ID */
    char path[MCP_MAX_PATH_LEN];    /* File path (ANSI); copy/move source */
    char dest[MCP_MAX_PATH_LEN];    /* copy/move destination path (ANSI) */
    char line[MCP_MAX_LINE];        /* Command line for exec (legacy; argv wins) */
    char data[MCP_MAX_DATA];        /* Base64 encoded file data */

    /* exec / ptyExec fields (Phase 4) */
    int  argv_count;                            /* 0 = argv absent */
    char argv[MCP_MAX_ARGV][MCP_MAX_ARG_LEN];   /* Parsed argv array */
    char cwd[MCP_MAX_PATH_LEN];                 /* Working directory ("" = inherit) */
    char stdin_b64[MCP_MAX_STDIN_B64];          /* Base64 stdin pass-through */
    int  timeout_ms;                /* 0 = server default (55000) */
    int  shell_flag;                /* "shell": true */
    int  unsafe_flag;               /* "unsafe": true (catalog bypass) */
    int  max_output;                /* 0 = cap (65536); clamps silently */
    int  mem_cap_bytes;             /* 0 = no cap; job objects only */
    int  cpu_time_ms;               /* 0 = no cap; job objects only */
    int  cols;                      /* ptyExec console width */
    int  rows;                      /* ptyExec console height */

    /* memory peek/poke fields (spec: memory-ops.allium) */
    char mem_token[MCP_MAX_MEM_TOKEN];  /* spawn-retain token (process tier) */
    char mem_addr[MCP_MAX_MEM_NUM];     /* address string; parsed by MemParseU32 */
    char mem_len[MCP_MAX_MEM_NUM];      /* length string; parsed by MemParseU32 */
} JsonCommand;

#endif /* COMMON_H */
