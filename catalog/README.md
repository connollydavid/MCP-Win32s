# Command catalog

`win32-commands.json` is the server-side **whitelist** and command-documentation
source for `mcp-w32s.exe`. It lets MCP clients discover what is safe to run when
`--help` is unavailable, and the server enforces it as an allow-list before any
`exec`/`ptyExec` spawns a child. It is loaded **once at startup** (changing it
requires a server restart) and converts 1:1 to MCP tool definitions
(see [`MCP-MAPPING.md`](MCP-MAPPING.md)).

## Schema

```json
{
  "version": 1,
  "commands": {
    "<command-name>": {
      "kind":            "shell-builtin" | "external",
      "shell_modern":    "cmd.exe /c <name>",      // built-ins only
      "shell_win32s":    "command.com /c <name>",  // built-ins only
      "supports_win32s": true | false,
      "description":     "One-line description (faithful to MS docs).",
      "options": [
        {"flag": "/X", "desc": "Flag-only switch."},
        {"flag": "/Y", "arg": "value", "desc": "Flag that takes an argument."}
      ],
      "positional": [
        {"name": "param", "optional": true | false, "type": "path" | "string"}
      ],
      "examples": ["<name> ...", "<name> /X ..."]
    }
  }
}
```

### Field reference

| Field | Required | Meaning |
|---|---|---|
| `version` | yes | Schema version. Current value: `1`. |
| `commands` | yes | Object keyed by command name; each value is an entry. |
| `kind` | yes | `shell-builtin` (no `.exe`; wrapped in the era-correct shell) or `external` (a real program resolved via `PATH`). |
| `shell_modern` | built-ins | Shell wrapper on NT / Win 9x (`cmd.exe`). |
| `shell_win32s` | built-ins | Shell wrapper on Win 3.x + Win32s (`command.com`). |
| `supports_win32s` | yes | Whether the command is expected to work on the Win32s baseline. Advisory. |
| `description` | yes | One line, sourced from official Microsoft documentation. |
| `options` | yes | Array of accepted flags. May be empty (`[]`). |
| `options[].flag` | yes | The flag token, e.g. `/B`, `/TC`, `-Wall`. |
| `options[].arg` | optional | Present iff the flag takes a value; its string names the value for docs. |
| `options[].desc` | yes | Flag description, faithful to MS docs. |
| `positional` | yes | Array of positional parameters. May be empty (`[]`). |
| `positional[].name` | yes | Parameter name. |
| `positional[].optional` | yes | `false` → becomes an MCP `required` property. |
| `positional[].type` | yes | `path` or `string`. **Advisory** — not validated by the server. |
| `examples` | yes (≥2) | At least two example invocations. |

Built-in entries (`kind: "shell-builtin"`) must carry both `shell_modern` and
`shell_win32s`; external entries omit them. The era-correct shell is chosen at
runtime (Q9): `command.com` on Win32s, `cmd.exe` on NT/9x.

## Validation behaviour (`CatalogValidateArgs`)

The server validates the argv of every gated `exec`/`ptyExec` against the
matched entry's `options`:

- A token starting with `/` or `-` is a **flag** and must appear in `options`
  (matched **case-insensitively** on the flag name). An unknown flag is
  rejected with the error `argument not allowed`.
- A flag whose option declares an `arg`:
  - **split form** `/A value` — consumes the next token as its argument, but
    only when that token is not itself a flag;
  - **glued form** `/A:value` (or `/Avalue`, e.g. `/DWIN32`, `/Fefoo.exe`) — the
    value is part of the same token.
  Both forms validate **identically**.
- Tokens that are **not** flags are treated as **positionals** and are always
  allowed — `positional[].type` is advisory and the server performs no
  path-validity check.

## Server-side flags

| Flag | Effect |
|---|---|
| `/CATALOG:<path>` | Override the catalog file location. Default: `catalog/win32-commands.json` next to `mcp-w32s.exe`. |
| `/UNSAFE` (startup) | Disable whitelist **enforcement**. The catalog is still loaded and consulted for shell routing and `binary_type`, but uncatalogued commands are permitted to run. |
| `unsafe: true` (per request) | Bypass the whitelist for that one `exec`/`ptyExec`. The response reports `"unsafe_used": true` (never injected into `stderr`). |

A missing catalog at startup is **not** a fatal error: the server starts and
its ready message includes `"warning": "catalog not loaded"`. With no catalog
and enforcement on, no command is in the whitelist, so every gated request is
rejected with `command not in catalog` until a catalog is provided or `/UNSAFE`
is set.

## Extending the catalog

1. Add a new key under `commands` following the schema above.
2. Use a faithful one-line `description` and per-option `desc` from official
   Microsoft documentation.
3. Provide at least two `examples`.
4. Keep within the server's fixed limits: at most **64** commands
   (`CATALOG_MAX_ENTRIES`) and **16** options per command
   (`CATALOG_MAX_OPTIONS`); command names ≤ 63 chars, flags ≤ 31 chars.
5. Verify the file still parses:
   `python3 -c "import json; json.load(open('catalog/win32-commands.json'))"`,
   and that `tests/test_catalog.c` passes.
6. Confirm the new entry maps cleanly to an MCP tool under
   [`MCP-MAPPING.md`](MCP-MAPPING.md).
