//! Typed build parameters and the injection-safe argv/directive emission — the
//! pure-logic core of the `BuildArgvIsCatalogued` safety boundary
//! (`specs/mcp-bridge.allium`).
//!
//! The vendor-neutral param structs (`CompileParams` etc.) define each build
//! tool's inputSchema. `emit_argv` applies a definition's `ArgItem` template to
//! a param set: `argv[0]` is the role's catalogued command, every flag/directive
//! comes only from the template's literals, and each user value fills a `{}`
//! slot as exactly ONE argv element — never re-split, never able to introduce a
//! flag, a wlink directive, or a second command. The device's
//! `CatalogValidateArgs` + `ShellTailNeutralised` gate is the backstop.
//!
//! NO instruction-set policy: a param value (e.g. `extra_flags: ["/arch:SSE2"]`)
//! is the user's choice and is emitted verbatim. The project's i386/no-FP
//! constraints bind only `mcp-w32s.exe`'s own source, never user builds — see
//! `OBLIGATIONS-5.2.md` (recorded non-obligation).

use std::collections::BTreeMap;

use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::{Map, Value};

use crate::toolchain::definition::{ArgItem, ToolRole};

/// A resolved parameter value, by kind, as the template references it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParamValue {
    /// An optional scalar (`output`, `warning_level`, `subsystem`).
    Text(Option<String>),
    /// A boolean (`compile_only`, `dll`, `debug`).
    Flag(bool),
    /// A list (`sources`, `includes`, `defines`, `objects`, `libs`, …).
    List(Vec<String>),
}

/// The param set an `ArgItem` template is applied to: param name → value.
pub type ParamMap = BTreeMap<String, ParamValue>;

/// `win32_<vendor>_compile` parameters.
#[derive(Debug, Clone, Default, Deserialize, JsonSchema)]
#[serde(default)]
#[schemars(deny_unknown_fields)]
pub struct CompileParams {
    /// Source files to compile (positional inputs).
    pub sources: Vec<String>,
    /// Output object/file path, if set.
    pub output: Option<String>,
    /// Include directories.
    pub includes: Vec<String>,
    /// Preprocessor defines (`NAME` or `NAME=value`).
    pub defines: Vec<String>,
    /// Warning level (toolchain-specific token, e.g. `3`).
    pub warning_level: Option<String>,
    /// Compile only, do not link.
    pub compile_only: bool,
    /// Extra raw flags passed verbatim — the user's full flag surface
    /// (optimization, processor/FP/SIMD target, anything the toolchain accepts).
    pub extra_flags: Vec<String>,
}

/// `win32_<vendor>_link` parameters.
#[derive(Debug, Clone, Default, Deserialize, JsonSchema)]
#[serde(default)]
#[schemars(deny_unknown_fields)]
pub struct LinkParams {
    /// Object files to link (positional inputs).
    pub objects: Vec<String>,
    /// Output image path, if set.
    pub output: Option<String>,
    /// Libraries to link.
    pub libs: Vec<String>,
    /// Library search paths.
    pub libpaths: Vec<String>,
    /// Subsystem token (e.g. `console`, `windows`/`nt`).
    pub subsystem: Option<String>,
    /// Produce a DLL.
    pub dll: bool,
    /// Emit debug info.
    pub debug: bool,
    /// Extra raw flags/directives passed verbatim.
    pub extra_flags: Vec<String>,
}

/// `win32_<vendor>_lib` parameters.
#[derive(Debug, Clone, Default, Deserialize, JsonSchema)]
#[serde(default)]
#[schemars(deny_unknown_fields)]
pub struct LibParams {
    /// Object files to archive (positional inputs).
    pub objects: Vec<String>,
    /// Output library path, if set.
    pub output: Option<String>,
    /// Extra raw flags passed verbatim.
    pub extra_flags: Vec<String>,
}

/// `win32_<vendor>_assemble` parameters.
#[derive(Debug, Clone, Default, Deserialize, JsonSchema)]
#[serde(default)]
#[schemars(deny_unknown_fields)]
pub struct AssembleParams {
    /// Source files to assemble (positional inputs).
    pub sources: Vec<String>,
    /// Output object path, if set.
    pub output: Option<String>,
    /// Include directories.
    pub includes: Vec<String>,
    /// Preprocessor defines.
    pub defines: Vec<String>,
    /// Extra raw flags passed verbatim.
    pub extra_flags: Vec<String>,
}

/// Convert a typed param struct into the generic `ParamMap` the template
/// references by name.
pub trait IntoParams {
    fn into_params(self) -> ParamMap;
}

impl IntoParams for CompileParams {
    fn into_params(self) -> ParamMap {
        unimplemented!("Agent C (argv): map fields onto ParamMap by their template names")
    }
}
impl IntoParams for LinkParams {
    fn into_params(self) -> ParamMap {
        unimplemented!("Agent C (argv): map fields onto ParamMap by their template names")
    }
}
impl IntoParams for LibParams {
    fn into_params(self) -> ParamMap {
        unimplemented!("Agent C (argv): map fields onto ParamMap by their template names")
    }
}
impl IntoParams for AssembleParams {
    fn into_params(self) -> ParamMap {
        unimplemented!("Agent C (argv): map fields onto ParamMap by their template names")
    }
}

/// The JSON Schema (2020-12, via `schemars`) for a role's parameters — the
/// generated tool's inputSchema.
pub fn role_schema(role: ToolRole) -> Value {
    unimplemented!("Agent C (argv): schemars::schema_for! per role, as a serde_json::Value")
}

/// Deserialize a tool call's raw arguments into the role's typed params and
/// then into the generic `ParamMap`. Errors (returned to the seam as a tool
/// error) on a schema mismatch.
pub fn parse_params(role: ToolRole, args: &Map<String, Value>) -> anyhow::Result<ParamMap> {
    unimplemented!("Agent C (argv): pick the role struct, deserialize, into_params")
}

/// Emit the argv for a build invocation. `argv[0]` is `command`; each `ArgItem`
/// in `template` contributes zero or more tokens, with every `{}` slot filled
/// by a single param value as exactly one argv element. THE injection boundary:
/// no user value is ever split on whitespace or re-interpreted as a flag, a
/// wlink directive, or a command separator. Returns the full argv (incl.
/// `argv[0]`).
pub fn emit_argv(
    command: &str,
    template: &[ArgItem],
    params: &ParamMap,
) -> anyhow::Result<Vec<String>> {
    unimplemented!("Agent C (argv): argv[0]=command; apply each ArgItem; {{}} = one element, never split")
}
