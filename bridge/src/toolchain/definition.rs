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
        unimplemented!("Agent A (definition): role commands + version_probe.command, deduped")
    }

    /// Does the detected version banner string satisfy this definition's
    /// support matrix? True iff some `supported_versions` entry is a prefix of
    /// `detected_version` (the spec's `toolchain_supported`). An empty matrix
    /// supports nothing (a definition with no declared builds generates no
    /// tool).
    pub fn supports(&self, detected_version: &str) -> bool {
        unimplemented!("Agent A (definition): prefix-match detected_version against supported_versions")
    }
}

/// The built-in MSVC reference definition (`cl`/`link`/`lib`/`ml`; the
/// `msvc_cc`/`msvc_link` dialects). Content from `docs/build-toolchain-flags.md`.
pub fn builtin_msvc() -> ToolchainDefinition {
    unimplemented!("Agent A (definition): the MSVC built-in, source = BuiltIn")
}

/// The built-in Open Watcom reference definition (`wcc386`/`wlink`/`wlib`/`wasm`;
/// the `watcom_cc`/`watcom_link` dialects, wlink directive emission).
pub fn builtin_watcom() -> ToolchainDefinition {
    unimplemented!("Agent A (definition): the Open Watcom built-in, source = BuiltIn")
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
    unimplemented!("Agent A (definition): deserialize + structural validation + stamp source")
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
        unimplemented!("Agent A (definition): new() + add the builtins()")
    }

    /// Add a definition (config or registered). A later definition with the
    /// same `name` replaces an earlier one.
    pub fn add(&mut self, def: ToolchainDefinition) {
        unimplemented!("Agent A (definition): insert/replace by name")
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
    pub fn validate_catalogued(
        &self,
        is_catalogued: impl Fn(&str) -> bool,
    ) -> anyhow::Result<()> {
        unimplemented!("Agent A (definition): every commands() entry passes is_catalogued, else error")
    }
}
