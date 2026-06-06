# Catalog → MCP tool mapping

This document defines the **1:1 mapping** from a `catalog/win32-commands.json`
entry to an [MCP](https://modelcontextprotocol.io/) *tool definition*. It is the
acceptance criterion recorded in Phase 4 ("MCP alignment decisions"): every
catalog entry must convert **mechanically** — no per-entry special-casing — to a
valid MCP tool so that the Phase 5 bridge can generate its tool list directly
from the catalog. This is documentation only; no mapping code ships in Phase 4.

## Mapping rules

| MCP tool field | Source in catalog entry | Rule |
|---|---|---|
| `name` | the entry **key** | Used verbatim (e.g. `dir`, `cl`). |
| `description` | `description` | Used verbatim. |
| `inputSchema.type` | — | Always the literal `"object"`. |
| `inputSchema.properties[flag]` | each `options[]` element | One property per option (see below). |
| `inputSchema.properties[name]` | each `positional[]` element | One property per positional (see below). |
| `inputSchema.required` | `positional[]` with `optional:false` | Array of the non-optional positional `name`s. Omitted when empty. |

### Option → property

For each element of `options[]`:

- **Property name:** the `flag` with its leading `/` or `-` stripped and the
  result lower-cased (e.g. `/B` → `b`, `/TC` → `tc`, `-Wall` → `wall`). This
  keeps property names valid JSON-Schema identifiers and stable across the two
  flag styles.
- **Property type:**
  - **No `arg` key** (flag-only switch) → `{"type": "boolean"}`. Presence of the
    flag on the command line corresponds to `true`.
  - **Has an `arg` key** (flag takes a value) → `{"type": "string"}`. The string
    is the flag's argument; both the split (`/A value`) and glued (`/A:value`)
    forms on the wire are accepted by the server (`CatalogValidateArgs`).
- **Property description:** the option's `desc`.

### Positional → property

For each element of `positional[]`:

- **Property name:** the `name` verbatim.
- **Property type:** `{"type": "string"}` for `type` of `string` or `path`
  (paths are strings on the wire; the catalog's `type` is advisory and is *not*
  re-encoded as a JSON-Schema format).
- **Property description:** derived from `name` and `type` (e.g.
  `"path (path)"`); positionals carry no `desc` in the catalog.
- **`required`:** the positional's `name` is added to `inputSchema.required`
  iff `optional` is `false`.

Fields `kind`, `shell_modern`, `shell_win32s`, `supports_win32s`, and
`examples` are **server-side / documentation** concerns and do **not** appear in
the MCP tool definition. (`kind`/`shell_*` drive era-correct shell routing;
`supports_win32s` and `examples` are advisory metadata.)

## Worked example: `dir`

Catalog entry:

```json
"dir": {
  "kind": "shell-builtin",
  "shell_modern": "cmd.exe /c dir",
  "shell_win32s": "command.com /c dir",
  "supports_win32s": true,
  "description": "Displays a list of files and subdirectories in a directory.",
  "options": [
    {"flag": "/A", "arg": "attrs", "desc": "Displays files with specified attributes (D R H A S I L); prefix with - to negate."},
    {"flag": "/B", "desc": "Uses bare format (no heading information or summary)."},
    {"flag": "/S", "desc": "Displays files in the specified directory and all subdirectories."},
    {"flag": "/O", "arg": "order", "desc": "Lists files in sorted order (N E S D G); prefix with - to reverse."},
    {"flag": "/W", "desc": "Uses wide list format."},
    {"flag": "/P", "desc": "Pauses after each screenful of information."}
  ],
  "positional": [{"name": "path", "optional": true, "type": "path"}],
  "examples": ["dir", "dir /B", "dir C:\\PROJECTS /S /B"]
}
```

Generated MCP tool definition:

```json
{
  "name": "dir",
  "description": "Displays a list of files and subdirectories in a directory.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "a": {"type": "string",  "description": "Displays files with specified attributes (D R H A S I L); prefix with - to negate."},
      "b": {"type": "boolean", "description": "Uses bare format (no heading information or summary)."},
      "s": {"type": "boolean", "description": "Displays files in the specified directory and all subdirectories."},
      "o": {"type": "string",  "description": "Lists files in sorted order (N E S D G); prefix with - to reverse."},
      "w": {"type": "boolean", "description": "Uses wide list format."},
      "p": {"type": "boolean", "description": "Pauses after each screenful of information."},
      "path": {"type": "string", "description": "path (path)"}
    }
  }
}
```

`path` is optional, so `inputSchema.required` is omitted. A tool whose entry has
a required positional (e.g. `type`, whose `file` is `optional:false`) emits
`"required": ["file"]`.

## Round-trip guarantee

Because the mapping is total and field-by-field, the bridge reconstructs a
command line from a tool call by the inverse:

1. Start with the tool `name`.
2. For each boolean property that is `true`, append its flag (recover the flag
   from the property name plus the catalog `options[]` entry).
3. For each string property derived from an option, append the flag and its
   value (split or glued, per the catalog).
4. Append positional values in `positional[]` order.

The server then re-validates the resulting argv with `CatalogValidateArgs`, so a
malformed reconstruction is rejected with `"argument not allowed"` rather than
executed.
