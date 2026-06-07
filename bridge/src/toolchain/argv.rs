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
        let mut params = ParamMap::new();
        params.insert("sources".into(), ParamValue::List(self.sources));
        params.insert("output".into(), ParamValue::Text(self.output));
        params.insert("includes".into(), ParamValue::List(self.includes));
        params.insert("defines".into(), ParamValue::List(self.defines));
        params.insert("warning_level".into(), ParamValue::Text(self.warning_level));
        params.insert("compile_only".into(), ParamValue::Flag(self.compile_only));
        params.insert("extra_flags".into(), ParamValue::List(self.extra_flags));
        params
    }
}
impl IntoParams for LinkParams {
    fn into_params(self) -> ParamMap {
        let mut params = ParamMap::new();
        params.insert("objects".into(), ParamValue::List(self.objects));
        params.insert("output".into(), ParamValue::Text(self.output));
        params.insert("libs".into(), ParamValue::List(self.libs));
        params.insert("libpaths".into(), ParamValue::List(self.libpaths));
        params.insert("subsystem".into(), ParamValue::Text(self.subsystem));
        params.insert("dll".into(), ParamValue::Flag(self.dll));
        params.insert("debug".into(), ParamValue::Flag(self.debug));
        params.insert("extra_flags".into(), ParamValue::List(self.extra_flags));
        params
    }
}
impl IntoParams for LibParams {
    fn into_params(self) -> ParamMap {
        let mut params = ParamMap::new();
        params.insert("objects".into(), ParamValue::List(self.objects));
        params.insert("output".into(), ParamValue::Text(self.output));
        params.insert("extra_flags".into(), ParamValue::List(self.extra_flags));
        params
    }
}
impl IntoParams for AssembleParams {
    fn into_params(self) -> ParamMap {
        let mut params = ParamMap::new();
        params.insert("sources".into(), ParamValue::List(self.sources));
        params.insert("output".into(), ParamValue::Text(self.output));
        params.insert("includes".into(), ParamValue::List(self.includes));
        params.insert("defines".into(), ParamValue::List(self.defines));
        params.insert("extra_flags".into(), ParamValue::List(self.extra_flags));
        params
    }
}

/// The JSON Schema (2020-12, via `schemars`) for a role's parameters — the
/// generated tool's inputSchema.
pub fn role_schema(role: ToolRole) -> Value {
    let schema = match role {
        ToolRole::Compile => schemars::schema_for!(CompileParams),
        ToolRole::Link => schemars::schema_for!(LinkParams),
        ToolRole::Lib => schemars::schema_for!(LibParams),
        ToolRole::Assemble => schemars::schema_for!(AssembleParams),
    };
    serde_json::to_value(schema).expect("a schemars schema serializes to a JSON value")
}

/// Deserialize a tool call's raw arguments into the role's typed params and
/// then into the generic `ParamMap`. Errors (returned to the seam as a tool
/// error) on a schema mismatch.
pub fn parse_params(role: ToolRole, args: &Map<String, Value>) -> anyhow::Result<ParamMap> {
    let value = Value::Object(args.clone());
    let params = match role {
        ToolRole::Compile => serde_json::from_value::<CompileParams>(value)?.into_params(),
        ToolRole::Link => serde_json::from_value::<LinkParams>(value)?.into_params(),
        ToolRole::Lib => serde_json::from_value::<LibParams>(value)?.into_params(),
        ToolRole::Assemble => serde_json::from_value::<AssembleParams>(value)?.into_params(),
    };
    Ok(params)
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
    let mut argv = vec![command.to_string()];
    for item in template {
        match item {
            ArgItem::Literal(s) => argv.push(s.clone()),
            ArgItem::Opt { opt, emit, then } => {
                if let Some(ParamValue::Text(Some(v))) = params.get(opt) {
                    argv.push(fill_slot(emit, v));
                    if let Some(then) = then {
                        argv.push(fill_slot(then, v));
                    }
                }
            }
            ArgItem::Flag { flag, emit } => {
                if let Some(ParamValue::Flag(true)) = params.get(flag) {
                    argv.push(emit.clone());
                }
            }
            ArgItem::Each {
                each,
                emit,
                positional: _,
            } => {
                if let Some(ParamValue::List(values)) = params.get(each) {
                    for v in values {
                        argv.push(fill_slot(emit, v));
                    }
                }
            }
            ArgItem::Join { join, sep } => {
                if let Some(ParamValue::List(values)) = params.get(join) {
                    if !values.is_empty() {
                        argv.push(values.join(sep));
                    }
                }
            }
        }
    }
    Ok(argv)
}

/// Fill the single `{}` slot in `template` with `value`, producing exactly one
/// argv token. The value is substituted verbatim into the slot — never split on
/// whitespace, never re-tokenised, never able to become a second argv element, a
/// flag, a `wlink` directive, or a command separator (the injection boundary).
/// A template with no `{}` is returned unchanged.
fn fill_slot(template: &str, value: &str) -> String {
    template.replacen("{}", value, 1)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The MSVC `compile` args template, copied verbatim from
    /// `docs/toolchain-definition-guide.md` (worked example 1, the `msvc_cc`
    /// dialect): glued `/I`/`/D`, `/W{}`, `/Fo{}`, verbatim `extra_flags`, then
    /// the positional sources.
    fn msvc_compile_template() -> Vec<ArgItem> {
        vec![
            ArgItem::Literal("/nologo".into()),
            ArgItem::Literal("/c".into()),
            ArgItem::Each {
                each: "includes".into(),
                emit: "/I{}".into(),
                positional: false,
            },
            ArgItem::Each {
                each: "defines".into(),
                emit: "/D{}".into(),
                positional: false,
            },
            ArgItem::Opt {
                opt: "warning_level".into(),
                emit: "/W{}".into(),
                then: None,
            },
            ArgItem::Opt {
                opt: "output".into(),
                emit: "/Fo{}".into(),
                then: None,
            },
            ArgItem::Each {
                each: "extra_flags".into(),
                emit: "{}".into(),
                positional: false,
            },
            ArgItem::Each {
                each: "sources".into(),
                emit: "{}".into(),
                positional: true,
            },
        ]
    }

    /// The Open Watcom `link` args template, copied verbatim from
    /// `docs/toolchain-definition-guide.md` (worked example 2, the wlink
    /// directive language): `option quiet`, `system <sub>`, `NAME <app>` via
    /// `then`, and the `FILE a.obj,b.obj` / `LIBRARY …` `Join` operands.
    fn watcom_link_template() -> Vec<ArgItem> {
        vec![
            ArgItem::Literal("option".into()),
            ArgItem::Literal("quiet".into()),
            ArgItem::Opt {
                opt: "subsystem".into(),
                emit: "system".into(),
                then: Some("{}".into()),
            },
            ArgItem::Opt {
                opt: "output".into(),
                emit: "name".into(),
                then: Some("{}".into()),
            },
            ArgItem::Literal("file".into()),
            ArgItem::Join {
                join: "objects".into(),
                sep: ",".into(),
            },
            ArgItem::Literal("library".into()),
            ArgItem::Join {
                join: "libs".into(),
                sep: ",".into(),
            },
        ]
    }

    /// `rule-success.BuildToolGenerated` data-flow + `entity-fields.RoleSpec`
    /// (the args template applied to `CompileParams`): the typed params reach
    /// the device's catalogued `exec` as the expected argv, `argv[0] == "cl"`
    /// (`BuildArgvIsCatalogued`).
    #[test]
    fn emit_argv_basic() {
        let params = CompileParams {
            sources: vec!["lexer.c".into(), "parser.c".into()],
            output: Some("out.obj".into()),
            includes: vec!["inc".into(), "shared".into()],
            defines: vec!["WIN32".into(), "NDEBUG".into()],
            warning_level: None,
            compile_only: false,
            extra_flags: vec!["/O2".into()],
        }
        .into_params();

        let argv = emit_argv("cl", &msvc_compile_template(), &params).unwrap();

        assert_eq!(
            argv,
            vec![
                "cl",
                "/nologo",
                "/c",
                "/Iinc",
                "/Ishared",
                "/DWIN32",
                "/DNDEBUG",
                "/Foout.obj",
                "/O2",
                "lexer.c",
                "parser.c",
            ]
        );
        assert_eq!(argv[0], "cl");
    }

    /// Safety pin #3 — `BuildArgvIsCatalogued` / `build_argv_is_injection_safe`
    /// (OBLIGATIONS-5.2.md, "Bridge: safety pins"). Values crafted to break out
    /// — a shell-metachar source `x&calc`, an output with a space, a
    /// quote/semicolon define, and a comma-bearing object fed into a Watcom
    /// `Join` — each must land as EXACTLY ONE argv element: never re-split,
    /// never a second command, a flag, or a `wlink` directive. Covers both the
    /// MSVC flag route and the Watcom wlink directive/Join route.
    #[test]
    fn build_argv_is_injection_safe() {
        /* MSVC flag route. */
        let params = CompileParams {
            sources: vec!["x&calc".into()],
            output: Some("a b.obj".into()),
            includes: vec![],
            defines: vec!["X=1\";evil".into()],
            warning_level: None,
            compile_only: false,
            extra_flags: vec![],
        }
        .into_params();

        let argv = emit_argv("cl", &msvc_compile_template(), &params).unwrap();

        /* argv[0] is always the role command, never a user value. */
        assert_eq!(argv[0], "cl");
        /* No element is a bare breakout token (a second command). */
        assert!(!argv.iter().any(|a| a == "calc"));
        assert!(!argv.iter().any(|a| a == "evil"));
        /* Each crafted value rides inside exactly ONE token, unsplit. */
        assert!(argv.contains(&"x&calc".to_string()));
        assert!(argv.contains(&"/Foa b.obj".to_string()));
        assert!(argv.contains(&"/DX=1\";evil".to_string()));
        /* Token count is exactly what the template emits — no extra elements
        from whitespace/metachar splitting: cl, /nologo, /c, /D…, /Fo…, source. */
        assert_eq!(argv.len(), 6);

        /* Open Watcom wlink directive + Join route. A comma-bearing object name
        must NOT split the FILE directive into extra operands, and the app name
        must NOT become a second wlink directive word. */
        let link = LinkParams {
            objects: vec!["a.obj,b.obj".into(), "c&d.obj".into()],
            output: Some("app name".into()),
            libs: vec!["kernel32.lib".into()],
            libpaths: vec![],
            subsystem: Some("nt".into()),
            dll: false,
            debug: false,
            extra_flags: vec![],
        }
        .into_params();

        let largv = emit_argv("wlink", &watcom_link_template(), &link).unwrap();

        assert_eq!(largv[0], "wlink");
        /* The two objects join into ONE FILE operand by `,`; the embedded comma
        in the first object does not create extra operands — the whole list is a
        single token. */
        assert_eq!(
            largv,
            vec![
                "wlink",
                "option",
                "quiet",
                "system",
                "nt",
                "name",
                "app name",
                "file",
                "a.obj,b.obj,c&d.obj",
                "library",
                "kernel32.lib",
            ]
        );
        /* The crafted app name is one token (`name <app>` pair), never a
        directive word on its own. */
        let name_idx = largv.iter().position(|a| a == "name").unwrap();
        assert_eq!(largv[name_idx + 1], "app name");
        /* No object's content leaked out as a separate argv element. */
        assert!(!largv.iter().any(|a| a == "b.obj"));
        assert!(!largv.iter().any(|a| a == "c&d.obj"));
    }

    /// Recorded NON-OBLIGATION (OBLIGATIONS-5.2.md, "Non-obligations: No
    /// instruction-set policy"). `extra_flags` are the user's choice and are
    /// emitted VERBATIM — a CPU/FP/SIMD target flag is never filtered. A test
    /// asserting rejection would be WRONG; this asserts the opposite.
    #[test]
    fn no_instruction_set_policy() {
        let params = CompileParams {
            sources: vec!["a.c".into()],
            output: None,
            includes: vec![],
            defines: vec![],
            warning_level: None,
            compile_only: false,
            extra_flags: vec!["/arch:SSE2".into(), "/G6".into()],
        }
        .into_params();

        let argv = emit_argv("cl", &msvc_compile_template(), &params).unwrap();

        assert!(argv.contains(&"/arch:SSE2".to_string()));
        assert!(argv.contains(&"/G6".to_string()));

        /* The Watcom equivalents (`-bt=nt`, `-mf`) ride the same verbatim
        `extra_flags` path. */
        let watcom = CompileParams {
            sources: vec!["a.c".into()],
            output: None,
            includes: vec![],
            defines: vec![],
            warning_level: None,
            compile_only: false,
            extra_flags: vec!["-bt=nt".into(), "-mf".into()],
        }
        .into_params();
        let wargv = emit_argv(
            "wcc386",
            &[
                ArgItem::Literal("-zq".into()),
                ArgItem::Each {
                    each: "extra_flags".into(),
                    emit: "{}".into(),
                    positional: false,
                },
                ArgItem::Each {
                    each: "sources".into(),
                    emit: "{}".into(),
                    positional: true,
                },
            ],
            &watcom,
        )
        .unwrap();
        assert!(wargv.contains(&"-bt=nt".to_string()));
        assert!(wargv.contains(&"-mf".to_string()));
    }

    /// Tool-shape conformance (OBLIGATIONS-5.2.md, "Bridge: end-to-end +
    /// conformance"): the generated tool's `inputSchema` is a 2020-12 object
    /// schema with `additionalProperties:false` and the role's properties — the
    /// `deny_unknown_fields`-backed closed schema.
    #[test]
    fn role_schema_is_closed() {
        let schema = role_schema(ToolRole::Compile);
        let obj = schema.as_object().unwrap();

        assert_eq!(
            obj.get("$schema").and_then(Value::as_str),
            Some("https://json-schema.org/draft/2020-12/schema")
        );
        assert_eq!(obj.get("type").and_then(Value::as_str), Some("object"));
        assert_eq!(obj.get("additionalProperties"), Some(&Value::Bool(false)));

        let props = obj.get("properties").and_then(Value::as_object).unwrap();
        for name in [
            "sources",
            "output",
            "includes",
            "defines",
            "warning_level",
            "compile_only",
            "extra_flags",
        ] {
            assert!(props.contains_key(name), "missing property {name}");
        }
    }
}
