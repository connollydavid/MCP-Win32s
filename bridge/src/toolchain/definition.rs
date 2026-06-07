//! The declarative `ToolchainDefinition` schema — toolchains as author-supplied
//! DATA — plus the loader, the built-in MSVC / Open Watcom reference
//! definitions, and the registry that holds them. Models `specs/toolchains.allium`
//! (`ToolchainDefinition`, `RoleSpec`, `ToolRole`, `DefinitionSource`, and the
//! `DefinitionCommandsAreCatalogued` safety floor).
//!
//! A new toolchain needs a definition, not bridge code. The authoring format is
//! documented in `docs/toolchain-definition-guide.md`; MSVC and Open Watcom are
//! the two built-in definitions in exactly that format.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::toolchain::diagnostics::Severity;

/// The four build-pipeline steps a toolchain may provide. A definition supplies
/// a `RoleSpec` per role it implements; a missing role yields no tool.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ToolRole {
    Compile,
    Link,
    Lib,
    Assemble,
}

impl ToolRole {
    /// The lowercase role token used in the generated tool name
    /// (`win32_<vendor>_<role>`) and in diagnostics.
    pub fn as_str(self) -> &'static str {
        match self {
            ToolRole::Compile => "compile",
            ToolRole::Link => "link",
            ToolRole::Lib => "lib",
            ToolRole::Assemble => "assemble",
        }
    }
}

/// Where a definition came from. `built_in` ships with the bridge (MSVC, Open
/// Watcom); `config` is an operator file loaded at startup; `registered`
/// arrives at runtime via `win32_register_toolchain` (operator opt-in).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DefinitionSource {
    BuiltIn,
    Config,
    Registered,
}

/// One item in a role's `args` template (the typed-param → argv/directive
/// mini-language; see the authoring guide). `{}` in an `emit`/`then` string is
/// the single substitution slot, filled with one user value as one argv element
/// — never re-split, never shell/directive-interpreted (the injection boundary,
/// `argv::emit_argv`). Deserialized untagged: a bare string is a `Literal`; an
/// object is discriminated by its key (`opt`/`flag`/`each`/`join`).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(untagged)]
pub enum ArgItem {
    /// Emit the literal token verbatim (e.g. `"/nologo"`).
    Literal(String),
    /// If the named scalar param is set, emit one token with `{}` replaced by
    /// its value. `then` emits a SECOND token after it (the `wlink NAME <app>`
    /// directive-pair form), again `{}`-substituted.
    Opt {
        opt: String,
        emit: String,
        #[serde(default)]
        then: Option<String>,
    },
    /// Emit `emit` verbatim only if the named boolean param is true.
    Flag { flag: String, emit: String },
    /// Emit one token per element of the named list param, `{}` = the element.
    /// `positional` inputs carry no flag (the bare source/object operands).
    Each {
        each: String,
        emit: String,
        #[serde(default)]
        positional: bool,
    },
    /// Emit the named list param joined into ONE token by `sep` (the wlink
    /// `FILE a.obj,b.obj` directive operand form).
    Join { join: String, sep: String },
}

/// A toolchain's diagnostic dialect: the regex applied to its output and the
/// keyword→severity normalization. The regex uses named captures `file`,
/// `line`, `column`, `severity`, `code`, `message` (all but `code`/`message`
/// optional). Compiled bounded by `diagnostics::BoundedRegex`.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct DiagnosticSpec {
    pub regex: String,
    #[serde(default)]
    pub severity_map: BTreeMap<String, Severity>,
}

/// One role within a definition: the catalogued program that implements it, the
/// args template that maps typed params to its argv/directives, and the
/// diagnostic dialect that parses its output.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct RoleSpec {
    pub command: String,
    #[serde(default)]
    pub args: Vec<ArgItem>,
    pub diagnostic: DiagnosticSpec,
}

/// How to read the installed version: run `command` (with `args`, typically
/// bare so the banner prints) and extract the version with `version_regex`
/// (a named `version` capture).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct VersionProbe {
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    pub version_regex: String,
}

/// A declarative toolchain description — the unit an operator or agent authors.
/// `source` is set by the loader (not the JSON). `supported_versions` is the
/// build-number-granular allow-list, prefix-matched against the detected
/// version (so VC6 `12.00.8168` and `12.00.8804` are distinct entries).
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct ToolchainDefinition {
    pub name: String,
    pub vendor: String,
    #[serde(skip, default = "default_source")]
    pub source: DefinitionSource,
    pub version_probe: VersionProbe,
    #[serde(default)]
    pub supported_versions: Vec<String>,
    pub roles: BTreeMap<ToolRole, RoleSpec>,
}

fn default_source() -> DefinitionSource {
    DefinitionSource::Config
}

impl ToolchainDefinition {
    /// Every catalogued program this definition can drive: each role's
    /// `command` plus the `version_probe.command`. The set
    /// `DefinitionCommandsAreCatalogued` must find in the device catalog.
    pub fn commands(&self) -> Vec<&str> {
        let mut out: Vec<&str> = Vec::new();
        for role in self.roles.values() {
            if !out.contains(&role.command.as_str()) {
                out.push(role.command.as_str());
            }
        }
        let probe = self.version_probe.command.as_str();
        if !out.contains(&probe) {
            out.push(probe);
        }
        out
    }

    /// Does the detected version banner string satisfy this definition's
    /// support matrix? True iff some `supported_versions` entry is a prefix of
    /// `detected_version` (the spec's `toolchain_supported`). An empty matrix
    /// supports nothing (a definition with no declared builds generates no
    /// tool).
    pub fn supports(&self, detected_version: &str) -> bool {
        self.supported_versions
            .iter()
            .any(|v| detected_version.starts_with(v.as_str()))
    }
}

/// A literal template item (`"/nologo"`).
fn lit(s: &str) -> ArgItem {
    ArgItem::Literal(s.to_string())
}

/// An `opt` template item: emit `emit` (with `{}` filled) when the scalar
/// param is set.
fn opt(param: &str, emit: &str) -> ArgItem {
    ArgItem::Opt {
        opt: param.to_string(),
        emit: emit.to_string(),
        then: None,
    }
}

/// An `each` template item: one token per list element, `{}` = the element.
fn each(param: &str, emit: &str) -> ArgItem {
    ArgItem::Each {
        each: param.to_string(),
        emit: emit.to_string(),
        positional: false,
    }
}

/// A positional `each` item (bare operands, no flag).
fn positional(param: &str) -> ArgItem {
    ArgItem::Each {
        each: param.to_string(),
        emit: "{}".to_string(),
        positional: true,
    }
}

/// A `severity_map` from `(keyword, severity)` pairs.
fn severity_map(pairs: &[(&str, Severity)]) -> BTreeMap<String, Severity> {
    pairs.iter().map(|(k, v)| (k.to_string(), *v)).collect()
}

/// The built-in MSVC reference definition (`cl`/`link`/`lib`/`ml`; the
/// `msvc_cc`/`msvc_link` dialects). Content from `docs/build-toolchain-flags.md`.
pub fn builtin_msvc() -> ToolchainDefinition {
    /* msvc_cc: `file(line[,col]): severity Cxxxx/Axxxx: msg` (col optional). */
    let msvc_cc = DiagnosticSpec {
        regex: r"^(?<file>.+?)\((?<line>\d+)(?:,(?<column>\d+))?\)\s*:\s*(?<severity>error|warning|fatal error)\s+(?<code>[A-Za-z]+\d+)\s*:\s*(?<message>.*)$".to_string(),
        severity_map: severity_map(&[
            ("fatal error", Severity::Fatal),
            ("error", Severity::Error),
            ("warning", Severity::Warning),
        ]),
    };
    /* msvc_link: the two LNK#### forms (`obj : error LNK2019: …` and
    `LINK : fatal error LNK1104: …`); link-level errors carry no source
    position, so no line/column captures. */
    let msvc_link = DiagnosticSpec {
        regex: r"^(?<file>.+?)\s*:\s*(?<severity>error|warning|fatal error)\s+(?<code>LNK\d+)\s*:\s*(?<message>.*)$".to_string(),
        severity_map: severity_map(&[
            ("fatal error", Severity::Fatal),
            ("error", Severity::Error),
            ("warning", Severity::Warning),
        ]),
    };

    let mut roles = BTreeMap::new();
    roles.insert(
        ToolRole::Compile,
        RoleSpec {
            command: "cl".to_string(),
            args: vec![
                lit("/nologo"),
                lit("/c"),
                each("includes", "/I{}"),
                each("defines", "/D{}"),
                opt("warning_level", "/W{}"),
                opt("output", "/Fo{}"),
                each("extra_flags", "{}"),
                positional("sources"),
            ],
            diagnostic: msvc_cc.clone(),
        },
    );
    roles.insert(
        ToolRole::Assemble,
        RoleSpec {
            command: "ml".to_string(),
            args: vec![
                lit("/nologo"),
                lit("/c"),
                each("includes", "/I{}"),
                each("defines", "/D{}"),
                opt("output", "/Fo{}"),
                each("extra_flags", "{}"),
                positional("sources"),
            ],
            diagnostic: msvc_cc,
        },
    );
    roles.insert(
        ToolRole::Link,
        RoleSpec {
            command: "link".to_string(),
            args: vec![
                lit("/NOLOGO"),
                ArgItem::Flag {
                    flag: "dll".to_string(),
                    emit: "/DLL".to_string(),
                },
                ArgItem::Flag {
                    flag: "debug".to_string(),
                    emit: "/DEBUG".to_string(),
                },
                opt("subsystem", "/SUBSYSTEM:{}"),
                opt("output", "/OUT:{}"),
                each("libpaths", "/LIBPATH:{}"),
                each("extra_flags", "{}"),
                positional("objects"),
                positional("libs"),
            ],
            diagnostic: msvc_link.clone(),
        },
    );
    roles.insert(
        ToolRole::Lib,
        RoleSpec {
            command: "lib".to_string(),
            args: vec![
                lit("/NOLOGO"),
                opt("output", "/OUT:{}"),
                each("extra_flags", "{}"),
                positional("objects"),
            ],
            diagnostic: msvc_link,
        },
    );

    ToolchainDefinition {
        name: "msvc".to_string(),
        vendor: "Microsoft".to_string(),
        source: DefinitionSource::BuiltIn,
        version_probe: VersionProbe {
            command: "cl".to_string(),
            args: vec![],
            version_regex: r"Version (?<version>\d+\.\d+\.\d+)".to_string(),
        },
        supported_versions: vec![
            "12.00.8168".to_string(),
            "12.00.8804".to_string(),
            "19.".to_string(),
        ],
        roles,
    }
}

/// The built-in Open Watcom reference definition (`wcc386`/`wlink`/`wlib`/`wasm`;
/// the `watcom_cc`/`watcom_link` dialects, wlink directive emission).
pub fn builtin_watcom() -> ToolchainDefinition {
    /* watcom_cc: `file(line): Error!/Warning! E####/W###: msg` — exclamation
    keyword, no column. */
    let watcom_cc = DiagnosticSpec {
        regex: r"^(?<file>.+?)\((?<line>\d+)\): (?<severity>Error|Warning)! (?<code>[EW]\d+): (?<message>.*)$".to_string(),
        severity_map: severity_map(&[
            ("Error", Severity::Error),
            ("Warning", Severity::Warning),
        ]),
    };
    /* watcom_link: bare 4-digit codes (`1014 stack segment not found`). */
    let watcom_link = DiagnosticSpec {
        regex: r"^(?<code>\d{4}) (?<message>.*)$".to_string(),
        severity_map: BTreeMap::new(),
    };

    let mut roles = BTreeMap::new();
    roles.insert(
        ToolRole::Compile,
        RoleSpec {
            command: "wcc386".to_string(),
            args: vec![
                lit("-zq"),
                each("includes", "-i={}"),
                each("defines", "-d{}"),
                opt("output", "-fo={}"),
                lit("-bt=nt"),
                lit("-mf"),
                each("extra_flags", "{}"),
                positional("sources"),
            ],
            diagnostic: watcom_cc.clone(),
        },
    );
    roles.insert(
        ToolRole::Assemble,
        RoleSpec {
            command: "wasm".to_string(),
            args: vec![
                lit("-zq"),
                each("includes", "-i={}"),
                each("defines", "-d{}"),
                opt("output", "-fo={}"),
                each("extra_flags", "{}"),
                positional("sources"),
            ],
            diagnostic: watcom_cc,
        },
    );
    roles.insert(
        ToolRole::Link,
        RoleSpec {
            command: "wlink".to_string(),
            args: vec![
                lit("option"),
                lit("quiet"),
                lit("system"),
                lit("nt"),
                ArgItem::Opt {
                    opt: "output".to_string(),
                    emit: "name".to_string(),
                    then: Some("{}".to_string()),
                },
                lit("file"),
                ArgItem::Join {
                    join: "objects".to_string(),
                    sep: ",".to_string(),
                },
                lit("library"),
                ArgItem::Join {
                    join: "libs".to_string(),
                    sep: ",".to_string(),
                },
                each("extra_flags", "{}"),
            ],
            diagnostic: watcom_link.clone(),
        },
    );
    roles.insert(
        ToolRole::Lib,
        RoleSpec {
            command: "wlib".to_string(),
            args: vec![
                opt("output", "-o={}"),
                each("extra_flags", "{}"),
                each("objects", "+{}"),
            ],
            diagnostic: watcom_link,
        },
    );

    ToolchainDefinition {
        name: "watcom".to_string(),
        vendor: "Open Watcom".to_string(),
        source: DefinitionSource::BuiltIn,
        version_probe: VersionProbe {
            command: "wcc386".to_string(),
            args: vec![],
            version_regex: r"Version (?<version>\d+\.\d+)".to_string(),
        },
        supported_versions: vec!["1.9".to_string(), "2.".to_string()],
        roles,
    }
}

/// The two built-in definitions.
pub fn builtins() -> Vec<ToolchainDefinition> {
    vec![builtin_msvc(), builtin_watcom()]
}

/// Load and validate a definition from JSON, stamping its `source`. Rejects a
/// structurally-invalid definition; catalog-membership is checked separately by
/// `Registry::validate_catalogued` (the loader does not know the device
/// catalog).
pub fn from_json(
    value: &serde_json::Value,
    source: DefinitionSource,
) -> anyhow::Result<ToolchainDefinition> {
    let mut def: ToolchainDefinition = serde_json::from_value(value.clone())?;
    def.source = source;
    Ok(def)
}

/// The set of loaded definitions (built-in + config + registered) the bridge
/// generates tools from.
#[derive(Debug, Clone, Default)]
pub struct Registry {
    definitions: Vec<ToolchainDefinition>,
}

impl Registry {
    /// An empty registry.
    pub fn new() -> Self {
        Registry::default()
    }

    /// A registry seeded with the two built-in definitions.
    pub fn with_builtins() -> Self {
        let mut registry = Registry::new();
        for def in builtins() {
            registry.add(def);
        }
        registry
    }

    /// Add a definition (config or registered). A later definition with the
    /// same `name` replaces an earlier one.
    pub fn add(&mut self, def: ToolchainDefinition) {
        if let Some(existing) = self.definitions.iter_mut().find(|d| d.name == def.name) {
            *existing = def;
        } else {
            self.definitions.push(def);
        }
    }

    /// All loaded definitions.
    pub fn definitions(&self) -> &[ToolchainDefinition] {
        &self.definitions
    }

    /// The safety floor (`DefinitionCommandsAreCatalogued`): every command every
    /// definition can drive is device-catalogued. `is_catalogued` answers
    /// membership in the device catalog (the registry stays decoupled from the
    /// catalog source). Errors naming the first offending command if any
    /// definition references an uncatalogued program — authorship can never
    /// introduce a new executable.
    pub fn validate_catalogued(&self, is_catalogued: impl Fn(&str) -> bool) -> anyhow::Result<()> {
        for def in &self.definitions {
            for command in def.commands() {
                if !is_catalogued(command) {
                    anyhow::bail!(
                        "toolchain definition '{}' references uncatalogued command '{}'",
                        def.name,
                        command
                    );
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The eight catalogued programs the two built-ins drive (the device
    /// catalog entries that let `DefinitionCommandsAreCatalogued` hold —
    /// `OBLIGATIONS-5.2.md`).
    const CATALOGUED_BUILTIN_COMMANDS: &[&str] =
        &["cl", "link", "lib", "ml", "wcc386", "wlink", "wlib", "wasm"];

    fn builtin_commands_catalogued(command: &str) -> bool {
        CATALOGUED_BUILTIN_COMMANDS.contains(&command)
    }

    /// entity-fields.ToolchainDefinition / entity-fields.RoleSpec,
    /// enum-comparable.ToolRole / enum-comparable.DefinitionSource:
    /// both built-ins build; a JSON definition round-trips through `from_json`;
    /// every untagged `ArgItem` form (literal/opt/flag/each/join) deserializes.
    #[test]
    fn definition_schema_roundtrips() {
        /* The two built-in definitions build and carry source = BuiltIn. */
        let msvc = builtin_msvc();
        assert_eq!(msvc.name, "msvc");
        assert_eq!(msvc.source, DefinitionSource::BuiltIn);
        assert_eq!(msvc.roles.len(), 4);
        let watcom = builtin_watcom();
        assert_eq!(watcom.name, "watcom");
        assert_eq!(watcom.source, DefinitionSource::BuiltIn);
        assert_eq!(watcom.roles.len(), 4);

        /* enum-comparable.ToolRole: every role token deserializes. */
        for (token, role) in [
            ("compile", ToolRole::Compile),
            ("link", ToolRole::Link),
            ("lib", ToolRole::Lib),
            ("assemble", ToolRole::Assemble),
        ] {
            let parsed: ToolRole =
                serde_json::from_value(serde_json::Value::String(token.to_string())).unwrap();
            assert_eq!(parsed, role);
        }

        /* enum-comparable.DefinitionSource: every source token deserializes. */
        for (token, src) in [
            ("built_in", DefinitionSource::BuiltIn),
            ("config", DefinitionSource::Config),
            ("registered", DefinitionSource::Registered),
        ] {
            let parsed: DefinitionSource =
                serde_json::from_value(serde_json::Value::String(token.to_string())).unwrap();
            assert_eq!(parsed, src);
        }

        /* A JSON definition exercising all five untagged ArgItem forms:
        a bare string (Literal), opt+then, flag, each (with positional),
        and join. */
        let json = serde_json::json!({
            "name": "demo",
            "vendor": "Demo Co",
            "version_probe": {
                "command": "cl",
                "args": [],
                "version_regex": "Version (?<version>\\d+\\.\\d+\\.\\d+)"
            },
            "supported_versions": ["1."],
            "roles": {
                "compile": {
                    "command": "cl",
                    "args": [
                        "/nologo",
                        { "opt": "output", "emit": "/Fo{}" },
                        { "flag": "compile_only", "emit": "/c" },
                        { "each": "includes", "emit": "/I{}" },
                        { "each": "sources", "emit": "{}", "positional": true }
                    ],
                    "diagnostic": {
                        "regex": "^(?<code>C\\d+): (?<message>.*)$",
                        "severity_map": { "error": "error" }
                    }
                },
                "link": {
                    "command": "link",
                    "args": [
                        { "opt": "output", "emit": "name", "then": "{}" },
                        { "join": "objects", "sep": "," }
                    ],
                    "diagnostic": {
                        "regex": "^(?<code>LNK\\d+): (?<message>.*)$"
                    }
                }
            }
        });

        let def = from_json(&json, DefinitionSource::Config).unwrap();
        /* from_json stamps source (it is `#[serde(skip)]` in the JSON). */
        assert_eq!(def.source, DefinitionSource::Config);
        assert_eq!(def.name, "demo");

        let compile = &def.roles[&ToolRole::Compile];
        assert_eq!(
            compile.args,
            vec![
                ArgItem::Literal("/nologo".to_string()),
                ArgItem::Opt {
                    opt: "output".to_string(),
                    emit: "/Fo{}".to_string(),
                    then: None,
                },
                ArgItem::Flag {
                    flag: "compile_only".to_string(),
                    emit: "/c".to_string(),
                },
                ArgItem::Each {
                    each: "includes".to_string(),
                    emit: "/I{}".to_string(),
                    positional: false,
                },
                ArgItem::Each {
                    each: "sources".to_string(),
                    emit: "{}".to_string(),
                    positional: true,
                },
            ]
        );

        let link = &def.roles[&ToolRole::Link];
        assert_eq!(
            link.args,
            vec![
                ArgItem::Opt {
                    opt: "output".to_string(),
                    emit: "name".to_string(),
                    then: Some("{}".to_string()),
                },
                ArgItem::Join {
                    join: "objects".to_string(),
                    sep: ",".to_string(),
                },
            ]
        );

        /* entity-optional: a diagnostic with no severity_map defaults to empty. */
        assert!(link.diagnostic.severity_map.is_empty());
    }

    /// invariant.DefinitionCommandsAreCatalogued (SAFETY PIN):
    /// with `validate_catalogued`, the two built-ins pass when their commands
    /// are catalogued; a definition naming an UNCATALOGUED command is rejected.
    /// Pins that authorship can never introduce an uncatalogued command.
    #[test]
    fn definition_commands_must_be_catalogued() {
        /* Each built-in's commands() are exactly the catalogued set, deduped
        (version_probe.command `cl`/`wcc386` already appears as a role
        command). Order follows the role enum (compile, link, lib, assemble). */
        let msvc = builtin_msvc();
        assert_eq!(msvc.commands(), vec!["cl", "link", "lib", "ml"]);
        let watcom = builtin_watcom();
        assert_eq!(watcom.commands(), vec!["wcc386", "wlink", "wlib", "wasm"]);

        /* The built-in registry validates clean against a catalog holding
        exactly those eight commands. */
        let registry = Registry::with_builtins();
        assert_eq!(registry.definitions().len(), 2);
        registry
            .validate_catalogued(builtin_commands_catalogued)
            .expect("built-in commands are all catalogued");

        /* A definition naming an uncatalogued command is rejected — authorship
        cannot introduce a new executable. The fake catalog returns false
        for the smuggled `calc`. */
        let rogue = ToolchainDefinition {
            name: "rogue".to_string(),
            vendor: "Attacker".to_string(),
            source: DefinitionSource::Registered,
            version_probe: VersionProbe {
                command: "cl".to_string(),
                args: vec![],
                version_regex: r"Version (?<version>\d+)".to_string(),
            },
            supported_versions: vec!["1.".to_string()],
            roles: {
                let mut roles = BTreeMap::new();
                roles.insert(
                    ToolRole::Compile,
                    RoleSpec {
                        command: "calc".to_string(),
                        args: vec![],
                        diagnostic: DiagnosticSpec {
                            regex: r"^(?<code>X\d+): (?<message>.*)$".to_string(),
                            severity_map: BTreeMap::new(),
                        },
                    },
                );
                roles
            },
        };
        let mut registry = Registry::with_builtins();
        registry.add(rogue);
        let err = registry
            .validate_catalogued(builtin_commands_catalogued)
            .expect_err("an uncatalogued command must be refused");
        assert!(
            err.to_string().contains("calc"),
            "error must name the offending command: {err}"
        );
    }

    /// rule-failure.BuildToolGenerated.2 (toolchain_supported):
    /// prefix-match — `12.00.8804` is supported by MSVC; `13.10.x` is not;
    /// an empty support matrix supports nothing.
    #[test]
    fn supports_prefix_matches() {
        let msvc = builtin_msvc();
        /* A supported VC6 SP5 build number (exact entry). */
        assert!(msvc.supports("12.00.8804"));
        /* A modern cl banner — matched by the `19.` prefix. */
        assert!(msvc.supports("19.29.30133"));
        /* An unsupported version (no entry is a prefix of it). */
        assert!(!msvc.supports("13.10.3077"));

        /* An empty support matrix supports nothing — a definition with no
        declared builds generates no tool. */
        let mut empty = builtin_msvc();
        empty.supported_versions.clear();
        assert!(!empty.supports("12.00.8804"));
        assert!(!empty.supports(""));
    }
}
