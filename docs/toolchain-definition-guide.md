# Authoring a toolchain definition

This guide is for **agents and operators who want to teach the bridge a build
toolchain it doesn't already know** — a different compiler, a different
version, a cross-assembler, anything. You write a small declarative
**ToolchainDefinition** (JSON); the bridge turns it into `win32_<vendor>_<role>`
build tools, runs the toolchain through the device's catalogued `exec`, and
parses its diagnostics with the regex you supply. No bridge code changes.

MSVC and Open Watcom ship as **built-in definitions** in exactly this format —
they are reference examples, not special cases. The behavioural model behind
this format is `specs/toolchains.allium` (the schema) and
`specs/mcp-bridge.allium` (how tools are generated and gated); the exhaustive,
cited flag tables are in [`build-toolchain-flags.md`](build-toolchain-flags.md).

> The build tools enforce **no instruction-set policy.** A definition may
> expose any flag the toolchain accepts — target Pentium, x64, SSE2, the FPU,
> the VC6 Processor Pack, whatever. The project's own C89/i386/no-FP/Win32s
> rules constrain only the `mcp-w32s.exe` binary, never what users build.

## The two ways a definition is loaded

| Path | How | When | Gate |
|---|---|---|---|
| **Built-in** | ships with the bridge (`msvc`, `watcom`) | always | — |
| **Config** | an operator drops a `*.json` definition in the bridge's toolchains dir | startup | operator placed the file |
| **Registered** | an agent calls `win32_register_toolchain` with the definition inline | runtime | **operator opt-in** (`win32_register_toolchain` is only advertised when the operator enabled registration) + audit-logged |

Use `win32_list_toolchains` to see the schema, the loaded definitions, and the
toolchains the device actually detected.

## The non-negotiable safety rules

A definition produces the argv that runs on the device, so authorship rides
**inside** the existing security gate, never around it:

1. **Every `command` must be device-catalogued.** A definition can only drive a
   program already on the device's allow-list (`DefinitionCommandsAreCatalogued`).
   You cannot introduce a new executable through a definition. If your
   toolchain's program isn't catalogued, an operator must add it to the device
   catalog first — that is a deliberate, reviewed device-side step.
2. **Values fill slots; they never become syntax.** Call-time parameter values
   (paths, defines) and your template's literal flags are kept apart. The device
   neutralises any shell metacharacters in the user tail
   (`ShellTailNeutralised`), so a crafted filename can't inject a second command
   or a `wlink` directive (`BuildArgvIsCatalogued`).
3. **A diagnostic regex must be cheap to run.** It is applied to untrusted
   compiler output. Avoid catastrophic backtracking (nested quantifiers like
   `(.+)+`); the bridge bounds match time and will reject a runaway pattern.

## Schema

```jsonc
{
  "name": "msvc",                 // tool-name segment: win32_<name>_<role>
  "vendor": "Microsoft",          // human-facing
  "version_probe": {
    "command": "cl",              // a catalogued command
    "args": [],                   // run it bare to print its banner (do NOT pass /nologo)
    "version_regex": "Version (?<version>\\d+\\.\\d+\\.\\d+)"
  },
  "supported_versions": ["12.00.8168", "12.00.8804", "19."],  // prefix-matched against the detected version
  "roles": {
    "compile":  { "command": "cl",   "args": [ /* template, below */ ], "diagnostic": { /* below */ } },
    "link":     { "command": "link", "args": [ ... ], "diagnostic": { ... } },
    "lib":      { "command": "lib",  "args": [ ... ], "diagnostic": { ... } },
    "assemble": { "command": "ml",   "args": [ ... ], "diagnostic": { ... } }
  }
}
```

A role you omit simply yields no tool. `supported_versions` is the enumerated,
**build-number-granular** allow-list: a build tool is advertised only when the
device-detected version matches one of these prefixes (so VC6 `12.00.8168` and
`12.00.8804` are distinct entries — the cl banner build number is the only thing
that distinguishes service packs).

### The `args` template (typed params → argv)

Each role's `args` is a list of items applied in order. The typed parameters
come from the role's param struct (`CompileParams` etc.): `sources`,
`output`, `includes`, `defines`, `warning_level`, `compile_only`,
`extra_flags`, and for link `objects`, `libs`, `libpaths`, `subsystem`, `dll`,
`debug`.

| Item form | Meaning |
|---|---|
| `"literal"` | emit the literal token verbatim (e.g. `"/nologo"`) |
| `{ "opt": "<param>", "emit": "/Fo{}" }` | if the param is set, emit one token with `{}` replaced by its value |
| `{ "flag": "<bool param>", "emit": "/DLL" }` | emit the token only if the boolean param is true |
| `{ "each": "<list param>", "emit": "/I{}" }` | emit one token per element, `{}` = the element |
| `{ "each": "sources", "emit": "{}", "positional": true }` | positional inputs (no flag) |

`{}` is the only substitution; it is filled with a single user value as one argv
element (never re-split, never shell-interpreted).

### The `diagnostic` block

```jsonc
"diagnostic": {
  "regex": "^(?<file>.+?)\\((?<line>\\d+)(?:,(?<column>\\d+))?\\)\\s*:\\s*(?<severity>error|warning|fatal error)\\s+(?<code>[A-Za-z]+\\d+)\\s*:\\s*(?<message>.*)$",
  "severity_map": { "fatal error": "fatal", "error": "error", "warning": "warning" }
}
```

Named captures `file`, `line`, `column`, `severity`, `code`, `message` become
the structured `Diagnostic` fields; `line`/`column` are optional (omit them for
link-level errors). `severity_map` normalises the toolchain's keyword onto
`error | warning | fatal | info`. The bridge applies the regex line-by-line to
**both** stdout and stderr (the stream split isn't reliable). A compile error
is **not** a tool error: when the toolchain ran, the tool returns
`isError:false` with `{ success, exit_code, diagnostics }` — `success` is
`exit_code == 0`.

## Worked example 1 — MSVC `compile` (the `msvc_cc` dialect)

```jsonc
"compile": {
  "command": "cl",
  "args": [
    "/nologo", "/c",
    { "each": "includes",  "emit": "/I{}" },     // glued — VC6 forbids a space
    { "each": "defines",   "emit": "/D{}" },
    { "opt":  "warning_level", "emit": "/W{}" },
    { "opt":  "output",    "emit": "/Fo{}" },
    { "each": "extra_flags", "emit": "{}" },      // /O2, /arch:SSE2, /G6 … user's choice
    { "each": "sources",   "emit": "{}", "positional": true }
  ],
  "diagnostic": {
    "regex": "^(?<file>.+?)\\((?<line>\\d+)(?:,(?<column>\\d+))?\\)\\s*:\\s*(?<severity>error|warning|fatal error)\\s+(?<code>[A-Za-z]+\\d+)\\s*:\\s*(?<message>.*)$",
    "severity_map": { "fatal error": "fatal", "error": "error", "warning": "warning" }
  }
}
```

Matches `lexer.c(50): error C2065: 'tre': undeclared identifier` (VC6, no
column) and `lexer.c(50,20): error C2065: …` (modern cl with
`/diagnostics:column`). The same regex serves `assemble` (ml's `A####` codes
fit `[A-Za-z]+\d+`). The `link`/`lib` roles use the **`msvc_link`** dialect — a
different grammar (`obj : error LNK2019: …` and `LINK : fatal error LNK1104:
…`), so give them their own regex.

## Worked example 2 — Open Watcom deltas

Watcom is *not* MSVC-shaped, which is exactly why it's a definition and not a
hardcoded case. Two differences to note:

**Flags use `-name=value`:**

```jsonc
"compile": {
  "command": "wcc386",
  "args": [
    "-zq",
    { "each": "includes", "emit": "-i={}" },
    { "each": "defines",  "emit": "-d{}" },
    { "opt":  "output",   "emit": "-fo={}" },
    "-bt=nt", "-mf",                              // required for a Win32 build
    { "each": "extra_flags", "emit": "{}" },
    { "each": "sources",  "emit": "{}", "positional": true }
  ],
  "diagnostic": {
    "regex": "^(?<file>.+?)\\((?<line>\\d+)\\): (?<severity>Error|Warning)! (?<code>[EW]\\d+): (?<message>.*)$",
    "severity_map": { "Error": "error", "Warning": "warning" }
  }
}
```

Matches `err.c(9): Error! E1011: Symbol 'x' has not been declared`.

**`wlink` is a directive language, not flags** — the template emits directive
words, not `-`/`/` options:

```jsonc
"link": {
  "command": "wlink",
  "args": [
    "option", "quiet",
    "system", "nt",                               // or nt_win for a GUI app
    { "opt":  "output", "emit": "name", "then": "{}" },   // NAME <app>
    "file", { "join": "objects", "sep": "," },     // FILE a.obj,b.obj
    "library", { "join": "libs", "sep": "," }      // LIBRARY k.lib,user32.lib
  ],
  "diagnostic": {
    "regex": "^(?<code>\\d{4}) (?<message>.*)$",   // bare 4-digit codes
    "severity_map": {}
  }
}
```

(`then`/`join` are the directive-oriented template forms; see
`win32_list_toolchains` for the authoritative current schema.) Prefer
command-line directives over a `@file.lnk` — the bridge does not write temp
files on the device.

## Checklist before you register

- [ ] Every `command` is in the device catalog (`win32_list_commands`).
- [ ] `version_probe` runs the tool **bare** (no `/nologo`/`-zq`) so the banner prints.
- [ ] `supported_versions` lists the exact build prefixes you validated.
- [ ] Each role's `args` only ever puts user values through `{}` slots.
- [ ] Each `diagnostic.regex` is anchored (`^…$`), has the named captures, and
      has no nested-quantifier backtracking.
- [ ] You tested the regex against a real error line and a real warning line.
