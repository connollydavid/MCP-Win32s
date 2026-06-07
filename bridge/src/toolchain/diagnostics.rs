//! Diagnostic parsing for build tools: the normalized `Diagnostic` shape, the
//! `BuildOutcome` (a ran toolchain's structured result), and the bounded regex
//! that turns a toolchain's diagnostic lines into structured data.
//!
//! Models `specs/mcp-bridge.allium`: `Diagnostic`, `BuildResult`, `Severity`,
//! and the `BuildResultProduced`/`BuildDiagnosticRecorded` rules. The four
//! built-in dialects (`msvc_cc`, `msvc_link`, `watcom_cc`, `watcom_link`) are
//! not special-cased here — each is just a definition's `diagnostic.regex`
//! applied through the same `parse_diagnostics`.
//!
//! ReDoS safety: the regex is the Rust `regex` crate, which is linear-time by
//! construction (no catastrophic backtracking), compiled under explicit size
//! limits — an author-/agent-supplied pattern can never hang the bridge. See
//! `BoundedRegex::compile` and `OBLIGATIONS-5.2.md` `diagnostic_regex_is_bounded`.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

use crate::toolchain::definition::DiagnosticSpec;

/// Normalized diagnostic severity. Each definition's `severity_map` maps the
/// toolchain's own keyword (MSVC `error`/`warning`/`fatal error`; Open Watcom
/// `Error!`/`Warning!`) onto these; `info` covers note-level lines.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Severity {
    Error,
    Warning,
    Fatal,
    Info,
}

impl Severity {
    /// Resolve a toolchain keyword to a normalized severity via the
    /// definition's `severity_map`; an unmapped keyword falls back to `Info`
    /// (a note-level line that matched the regex but no known severity word).
    pub fn from_keyword(map: &BTreeMap<String, Severity>, keyword: &str) -> Severity {
        unimplemented!("Agent B (diagnostics): map keyword via severity_map, default Info")
    }
}

/// One parsed diagnostic line. `file`/`line`/`column` are absent for link-level
/// errors (no source position) and on toolchains whose format omits a column
/// (VC6, Open Watcom, modern cl by default). `code` is the toolchain's code
/// (Cxxxx/LNKxxxx/Axxxx/Exxxx/Wxxxx or a bare wlink number).
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Diagnostic {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub file: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub line: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub column: Option<i64>,
    pub severity: Severity,
    pub code: String,
    pub message: String,
}

/// The structured result of a build-tool call once the toolchain has RUN
/// (regardless of compile pass/fail) — it always rides an `is_error:false`
/// tool result. `success` reflects `exit_code == 0`; `dialect` names the parser
/// used (the role's diagnostic dialect, e.g. `msvc_cc`). Serialized as the
/// call's `structuredContent`. The toolchain failing to *run* is the only
/// is_error:true case and is handled by the seam, not produced here.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct BuildOutcome {
    pub success: bool,
    pub exit_code: i64,
    pub dialect: String,
    pub diagnostics: Vec<Diagnostic>,
}

/// A diagnostic regex compiled under size limits — the linear-time guard that
/// makes an author-supplied pattern safe to run on untrusted compiler output.
pub struct BoundedRegex {
    inner: regex::Regex,
}

impl BoundedRegex {
    /// The compiled-program size ceiling (bytes). A pattern whose compiled form
    /// exceeds this is rejected rather than run — the ReDoS/space bound.
    pub const SIZE_LIMIT: usize = 64 * 1024;

    /// Compile `pattern` with the size/DFA limits applied. Errors if the
    /// pattern is invalid or too large; never produces a regex that can
    /// backtrack catastrophically (the `regex` crate is linear-time).
    pub fn compile(pattern: &str) -> anyhow::Result<BoundedRegex> {
        unimplemented!("Agent B (diagnostics): RegexBuilder with size_limit/dfa_size_limit")
    }

    /// The underlying regex, for capture access in `parse_diagnostics`.
    pub fn as_regex(&self) -> &regex::Regex {
        &self.inner
    }
}

/// Parse a toolchain's output into structured diagnostics: compile
/// `spec.regex` (bounded), apply it line-by-line to BOTH `stdout` and `stderr`
/// (the stream split is behavioural/unpinned), and map the named captures
/// (`file`, `line`, `column`, `severity`, `code`, `message`) onto `Diagnostic`,
/// resolving severity through `spec.severity_map`. Lines that do not match are
/// skipped. Returns an error only if the regex fails to compile (a bad
/// definition), never on unmatched output.
pub fn parse_diagnostics(
    spec: &DiagnosticSpec,
    stdout: &str,
    stderr: &str,
) -> anyhow::Result<Vec<Diagnostic>> {
    unimplemented!("Agent B (diagnostics): bounded compile + line-by-line capture mapping")
}

/// Build the structured outcome of a ran toolchain: `success = (exit_code == 0)`
/// and the parsed diagnostics. This is the `BuildResultProduced` +
/// `BuildDiagnosticRecorded` logic; a nonzero compiler exit is still NOT a tool
/// error — the diagnostics ride the structuredContent of a successful call.
pub fn build_outcome(
    spec: &DiagnosticSpec,
    dialect: &str,
    exit_code: i64,
    stdout: &str,
    stderr: &str,
) -> anyhow::Result<BuildOutcome> {
    unimplemented!("Agent B (diagnostics): success=(exit_code==0), diagnostics=parse_diagnostics")
}
