# Build-toolchain flag reference (primary sources)

Cited primary-source grounding for the build-tool subsystem's argv/directive
emission and for the built-in toolchain definitions (`msvc`, `watcom`). Every
flag traces to an official source — Microsoft Learn / archived MSDN for MSVC,
the Open Watcom manuals for Watcom — not to recollection. This is the
reference the `ToolchainDefinition` flag-templates (and the device catalog
entries) are built from; see `toolchain-definition-guide.md` for how a
definition consumes it.

> **Scope boundary.** The project's own hard constraints (C89, i386, no-FP,
> no-SSE, Win32s) bind **only the source of `mcp-w32s.exe`**. They impose
> nothing on what users build through these tools — a user may target
> Pentium, x64, SSE2, the FPU, or the VC6 Processor Pack. The flag surface
> below is exposed in full; the build tools enforce **no instruction-set
> policy**. The only restriction is injection-safety (the command must be
> device-catalogued; the tail is neutralised).

The argv rule of thumb: **MSVC** — glue every value to its flag (`/Foout.obj`,
`/Ic:\inc`, `/DNAME=1`); this is the only form valid across VC6 *and* modern.
**Watcom** — option introducer is `-` or `/` (prefer `-`); values are glued or
`=` per the table.

---

## Microsoft MSVC

### Version landscape (the support set is build-number-granular)

`cl` has only **two distinct build numbers across VC6's whole SP1–SP6 life**:

| Release | `cl` build | cl changed? |
|---|---|---|
| VC6 RTM | `12.00.8168` | baseline |
| SP1–SP4 | `12.00.8168` | no (IDE/library/linker only; SP3 bumped `link` to `6.00.8447`) |
| SP5 | `12.00.8804` | **yes** — the single genuine cl rebuild |
| SP6 | `12.00.8804` | no (rollup; `_MSC_FULL_VER` 12008804) |
| Processor Pack (on SP4/SP5) | `12.00.8804` | adds FP/SIMD only (see below) |

`_MSC_VER` is `1200` for all of them and never distinguishes a service pack —
**the banner build number (`8168` vs `8804`) is the only discriminator.**
Modern MSVC is `cl 19.x` (VS2015–2022). Detection runs the tool with no input
(or `/?`) and reads the banner; **do not pass `/nologo` when probing** — it
suppresses the banner:

```
Microsoft (R) 32-bit C/C++ Optimizing Compiler Version 12.00.8804 for 80x86
```

The **Processor Pack** reports `8804` too (so `8804` alone does not imply PP)
and adds only FP/SIMD: `/QIfist`, SSE/SSE2/3DNow, P-III/P4 codegen. These are
the user's to use (not banned for *their* builds) but are PP-gated, never
assumed present. Sources:
[Microsoft Visual C++ — Wikipedia](https://en.wikipedia.org/wiki/Microsoft_Visual_C++),
[compiler versions](https://learn.microsoft.com/en-us/cpp/overview/compiler-versions?view=msvc-170),
[emsps version table](http://www.emsps.com/oldtools/mscppv.htm),
[VC6 Processor Pack (archive.org)](https://archive.org/details/vcpp5).

**Flag stability:** the core flags below are all present in RTM `8168` and
unchanged across the whole VC6 line and forward — so the flag-templates do not
fork by service pack. (Build-number strings are research-grounded; the device
reads the *actual* banner at runtime, so the support set is confirmed against
real installs during implement/acceptance.)

### `cl.exe` (compiler)

| Dimension | Token | Notes | Source |
|---|---|---|---|
| Compile only | `/c` | standalone | [/c](https://learn.microsoft.com/en-us/cpp/build/reference/c-compile-without-linking?view=msvc-170) |
| Object out | `/Fo<file>` | glued; **VC6 forbids a space** | [/Fo](https://learn.microsoft.com/en-us/cpp/build/reference/fo-object-file-name?view=msvc-170), [VC6 /Fo](https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-6.0/aa235433(v=vs.60)) |
| Exe out | `/Fe<file>` | glued; no effect with `/c` | [/Fe](https://learn.microsoft.com/en-us/cpp/build/reference/fe-name-exe-file?view=msvc-170) |
| Include dir | `/I<dir>` | space optional; glue to be safe | [/I](https://learn.microsoft.com/en-us/cpp/build/reference/i-additional-include-directories?view=msvc-170) |
| Define | `/D<name>[=<value>]` | no space around `=` | [/D](https://learn.microsoft.com/en-us/cpp/build/reference/d-preprocessor-definitions?view=msvc-170) |
| Warning level | `/W0`–`/W4`; `/Wall` (modern); `/WX` (as errors) | CLI default `/W1` | [/W…/WX](https://learn.microsoft.com/en-us/cpp/build/reference/compiler-option-warning-level?view=msvc-170) |
| Source as C/C++ | `/TC` / `/TP` (all); `/Tc<f>` / `/Tp<f>` (per file) | | [/Tc /Tp /TC /TP](https://learn.microsoft.com/en-us/cpp/build/reference/tc-tp-tc-tp-specify-source-file-type?view=msvc-170) |
| Optimize for speed | `/O2` | VC6 `/O2` = `/Ogityb1 /Gfy` | [/O1 /O2](https://learn.microsoft.com/en-us/cpp/build/reference/o1-o2-minimize-size-maximize-speed?view=msvc-170) |
| Suppress banner | `/nologo` | documented | [/nologo](https://learn.microsoft.com/en-us/cpp/build/reference/nologo-suppress-startup-banner-c-cpp?view=msvc-170) |
| Diagnostic column | `/diagnostics:column` | **modern only**; default is `classic` (no column) | [/diagnostics](https://learn.microsoft.com/en-us/cpp/build/reference/diagnostics-compiler-diagnostic-options?view=msvc-170) |

The **full processor/FP/optimization surface** (`/arch:*`, `/G*`, the PP's
`/QIfist`, the rest of `/O*`) is the user's to choose and is exposed.

### `ml.exe` / `ml64.exe` (MASM)

Options are case-sensitive. Source:
[ML and ML64 reference](https://learn.microsoft.com/en-us/cpp/assembler/masm/ml-and-ml64-command-line-reference?view=msvc-170).

| Dimension | Token | Notes |
|---|---|---|
| Assemble only | `/c` | standalone |
| Object out | `/Fo<file>` | glued; **must precede its `/c`** (ml accepts multiple `/c`) |
| Include path | `/I<path>` | glued; **max 10** |
| Define | `/D<symbol>[=<value>]` | glued |
| COFF output | `/coff` | **x86-only; required for Win32 asm; rejected by ml64** |

VC6 shipped MASM 6.x; `/coff` present then.

### `link.exe` (linker)

Colon form, case-insensitive; **object files and `.lib` libraries are
positional** (bare names). Sources:
[linker options](https://learn.microsoft.com/en-us/cpp/build/reference/linker-options?view=msvc-170),
[VC6 LINK](https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-6.0/aa270751(v=vs.60)).

| Dimension | Token |
|---|---|
| Output file | `/OUT:<file>` |
| Build a DLL | `/DLL` |
| Library search path | `/LIBPATH:<dir>` |
| Subsystem (+ version) | `/SUBSYSTEM:{CONSOLE\|WINDOWS\|…}[,major[.minor]]` (VC6 supports the suffix too) |
| Debug info | `/DEBUG` |
| Exclude default libs | `/NODEFAULTLIB[:<lib>]` |
| Suppress banner | `/NOLOGO` |

VC6 vs modern agree on every dimension above, including the `/SUBSYSTEM`
version suffix; the only divergence is keyword sets (`WINDOWSCE` present in VC6
LINK, gone in modern; modern adds `BOOT_APPLICATION`/`EFI_*`).

### `lib.exe` (library manager)

Source: [Managing a Library](https://learn.microsoft.com/en-us/cpp/build/reference/managing-a-library?view=msvc-170),
[Running LIB](https://learn.microsoft.com/en-us/cpp/build/reference/running-lib?view=msvc-170).

| Operation | Token |
|---|---|
| Name output | `/OUT:<file>` |
| Add objects | positional (bare `.obj`/`.lib`) |
| List contents | `/LIST` (to stdout) |
| Remove a member | `/REMOVE:<object>` |
| Extract a member | `/EXTRACT:<member>` (requires `/OUT:`; one per command) |
| Suppress banner | `/NOLOGO` |

---

## Open Watcom (32-bit / Win32)

Option introducer is `-` **or** `/` (prefer `-`); values glue or use `=` per
the table. The `wcl386` driver accepts all `wcc386` options plus the link-stage
ones; `wcc386` alone never links. v2 (2.x) and 1.9 agree on every dimension
below. Sources:
[OW 1.9 Tools User's Guide](https://open-watcom.github.io/open-watcom-1.9/tools.html),
[OW 1.9 Linker Guide](https://open-watcom.github.io/open-watcom-1.9/lguide.html),
[wlib](https://users.pja.edu.pl/~jms/qnx/help/watcom/compiler-tools/wlib.html),
[wasm](https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/cmn/wasm.gml),
[v2 C diagnostics](https://open-watcom.github.io/open-watcom-v2-wikidocs/wccerrs.html).

### `wcc386` / `wcl386` (compiler / driver)

| Dimension | Token | Notes |
|---|---|---|
| Compile only | `-c` | `wcl386` driver only (`wcc386` never links) |
| Object out | `-fo=<file>` | `=` form |
| Include dir | `-i=<dir>` | `=` form |
| Define | `-d<name>[=<text>]` | name glued, value via `=` |
| Warning level | `-w<number>` | glued (default `-w1`) |
| Warnings as errors | `-we` | |
| Source as C / C++ | `-cc` / `-cc++` | |
| Optimize speed / max | `-ot` / `-ox` | `-o*` letters glue (`-otexan`) |
| **Build target = NT** | `-bt=nt` | **required for a Win32 build** |
| **Memory model = flat** | `-mf` | **required for 32-bit** |
| Quiet (suppress banner) | `-zq` (`wcc386`) / `-q` (`wcl386`) | |
| Driver link target | `-l=nt` | `wcl386` only (driver-level link) |

The full processor/FP surface is likewise the user's choice and exposed.

### `wlink` (linker — directive language, not flags)

`wlink {directive}`; directives on the command line or in `@file.lnk`. **Prefer
command-line directives** over a temp `.lnk` (avoids a device temp-write
surface).

| Need | Directive |
|---|---|
| Output name | `NAME <app>` |
| Object files | `FILE obj1,obj2,…` |
| Libraries | `LIBRARY lib1,lib2,…` |
| Library path | `LIBPATH <path>` |
| Target — console | `SYSTEM nt` |
| Target — GUI | `SYSTEM nt_win` |
| Build a DLL | `FORMAT WINDOWS NT DLL` |
| Debug info | `DEBUG <kind>` (e.g. `DEBUG DWARF`) |
| Misc | `OPTION QUIET`, `OPTION MAP` |

Example: `wlink system nt name app.exe file app.obj,util.obj library kernel32.lib`.

### `wlib` (library manager)

`wlib [options] lib_file [cmd_list]`.

| Need | Token |
|---|---|
| Create / name output | `-o=<newlib>` |
| Add object | `+<obj>` |
| Delete module | `-<mod>` |
| Extract module | `*<mod>[=<file>]` (Win host uses `*`) |
| List contents | `-l[=<file>]` |

### `wasm` (assembler)

| Need | Token |
|---|---|
| Assemble only | default (no link stage exists) |
| Object out | `-fo=<file>` |
| Include path | `-i=<dir>` |
| Define | `-d<name>[=<text>]` |
| Quiet | `-zq` |

`wasm` emits **OMF** (no COFF switch; `wlink` consumes OMF).

---

## Diagnostic dialects (for the parser / `diagnostic_regex`)

Four built-in dialects. The compile-side and link-side grammars differ **within
a vendor** — that is why the dialect is keyed on (vendor × side), not version.
The diagnostic **stream** (stdout vs stderr) is behavioral and not pinned in
either vendor's docs, so **capture and parse both** streams.

| Dialect | Tools | Grammar · real example |
|---|---|---|
| `msvc_cc` | msvc compile, assemble | `file(line[,col]): severity Cxxxx/Axxxx: msg` — column optional. VC6 never has a column; **modern cl defaults to none** (opt in with `/diagnostics:column`). Severities `error`/`warning`/`fatal error`; prefixes `C` (compiler), `A` (MASM), `D` (command-line, e.g. `D9002`). Ex: `lexer.c(50): error C2065: 'tre': undeclared identifier` |
| `msvc_link` | msvc link, lib | shared `LNK####` namespace, two forms: `main.obj : error LNK2019: unresolved external symbol _foo referenced in function _main` and `LINK : fatal error LNK1104: cannot open file 'foo.lib'` (`LIB :` when lib is active). `LNK1xxx`=fatal, `LNK2xxx`=error, `LNK4xxx`=warning |
| `watcom_cc` | watcom compile, assemble | `file(line): Error!/Warning! E####/W###: msg` — exclamation keyword, no column. Ex: `err.c(9): Error! E1011: Symbol 'x' has not been declared`; `hello.c(5): Warning! W202: Symbol 'x' has been defined, but not referenced`. (The C++ compiler `wpp386` uses a different `(line,col)` form — not used here.) |
| `watcom_link` | watcom link | bare 4-digit codes (severity digit + 3), e.g. `1014 stack segment not found` — distinct grammar; parse separately from `watcom_cc` |

Sources: [/diagnostics](https://learn.microsoft.com/en-us/cpp/build/reference/diagnostics-compiler-diagnostic-options?view=msvc-170),
[C2065](https://learn.microsoft.com/en-us/cpp/error-messages/compiler-errors-1/compiler-error-c2065?view=msvc-170),
[linker tools errors](https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-errors-and-warnings?view=msvc-170),
[LNK2019](https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-error-lnk2019?view=msvc-170),
[LNK1104](https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/linker-tools-error-lnk1104?view=msvc-170),
[OW C diagnostics](https://open-watcom.github.io/open-watcom-v2-wikidocs/wccerrs.html).

---

## Undocumented / semi-documented flags

Surfaced because the build tools may need them and a definition may expose them
(the user noted "some may be undocumented"):

- **MSVC `cl`:** `/d1<x>` (pass to the front-end), `/d2<x>` (pass to the
  optimizer/back-end), and the timing sub-flags routed through them
  (`/d1reportTime`, `/d2cgsummary`/`/d2Timing`) are **undocumented** and
  version-volatile — do not treat as a stable contract. `/nologo`,
  `/showIncludes` are documented.
  [Geoff Chappell — CL options](https://www.geoffchappell.com/studies/msvc/cl/cl/options/index.htm),
  [Lectem — MSVC hidden flags](https://lectem.github.io/msvc/reverse-engineering/build/2019/01/21/MSVC-hidden-flags.html).
- **MSVC `link`:** `/Brepro`, `/emittoolversioninfo:no`, `/d2`, `/b1`, `/b2`,
  `/fullbuild`, `/nooptidata` and similar are **undocumented** (reproducible-build
  / internal controls). `/nologo`, `/VERBOSE` are documented.
  [Geoff Chappell — LINK options](https://www.geoffchappell.com/studies/msvc/link/link/options/index.htm),
  [assarbad/msvc-undoc](https://github.com/assarbad/msvc-undoc).
- **Open Watcom:** there is **no** diagnostic-format control flag — the
  `file(line): Error! E####:` form is fixed. `-zq` (quiet) is the key
  build-hygiene flag; `-zcm=<mode>` (wasm) is *syntax* compatibility, not an
  object-format switch (easy to misread). Option-introducer duality (`-`/`/`)
  holds across the tools — prefer `-` to avoid path/option ambiguity on Win32.
