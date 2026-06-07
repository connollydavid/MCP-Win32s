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
        map.get(keyword).copied().unwrap_or(Severity::Info)
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
        let inner = regex::RegexBuilder::new(pattern)
            .size_limit(Self::SIZE_LIMIT)
            .dfa_size_limit(Self::SIZE_LIMIT)
            .build()?;
        Ok(BoundedRegex { inner })
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
    let re = BoundedRegex::compile(&spec.regex)?;
    let re = re.as_regex();
    let mut diagnostics = Vec::new();
    for line in stdout.lines().chain(stderr.lines()) {
        let caps = match re.captures(line) {
            Some(caps) => caps,
            None => continue,
        };
        let file = caps.name("file").map(|m| m.as_str().to_string());
        let line_no = caps
            .name("line")
            .and_then(|m| m.as_str().parse::<i64>().ok());
        let column = caps
            .name("column")
            .and_then(|m| m.as_str().parse::<i64>().ok());
        let severity_kw = caps.name("severity").map(|m| m.as_str()).unwrap_or("");
        let severity = Severity::from_keyword(&spec.severity_map, severity_kw);
        let code = caps
            .name("code")
            .map(|m| m.as_str())
            .unwrap_or("")
            .to_string();
        let message = caps
            .name("message")
            .map(|m| m.as_str())
            .unwrap_or("")
            .to_string();
        diagnostics.push(Diagnostic {
            file,
            line: line_no,
            column,
            severity,
            code,
            message,
        });
    }
    Ok(diagnostics)
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
    let diagnostics = parse_diagnostics(spec, stdout, stderr)?;
    Ok(BuildOutcome {
        success: exit_code == 0,
        exit_code,
        dialect: dialect.to_string(),
        diagnostics,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The four built-in dialect regexes, copied verbatim from
    /// `docs/toolchain-definition-guide.md` (the `msvc_cc`, `watcom_cc`,
    /// `watcom_link` worked examples). `msvc_link` has no worked-example regex
    /// in the guide; it is authored here to the grammar the flags doc documents
    /// (`obj : error LNK2019: …` and `LINK : fatal error LNK1104: …`), carrying
    /// no `file`/`line`/`column` captures because link-level errors have no
    /// source position.
    const MSVC_CC_RE: &str = r"^(?<file>.+?)\((?<line>\d+)(?:,(?<column>\d+))?\)\s*:\s*(?<severity>error|warning|fatal error)\s+(?<code>[A-Za-z]+\d+)\s*:\s*(?<message>.*)$";
    const MSVC_LINK_RE: &str =
        r"^.+? : (?<severity>fatal error|error|warning) (?<code>LNK\d+): (?<message>.*)$";
    const WATCOM_CC_RE: &str = r"^(?<file>.+?)\((?<line>\d+)\): (?<severity>Error|Warning)! (?<code>[EW]\d+): (?<message>.*)$";
    const WATCOM_LINK_RE: &str = r"^(?<code>\d{4}) (?<message>.*)$";

    fn sev(pairs: &[(&str, Severity)]) -> BTreeMap<String, Severity> {
        pairs.iter().map(|(k, v)| (k.to_string(), *v)).collect()
    }

    fn msvc_cc_spec() -> DiagnosticSpec {
        DiagnosticSpec {
            regex: MSVC_CC_RE.to_string(),
            severity_map: sev(&[
                ("fatal error", Severity::Fatal),
                ("error", Severity::Error),
                ("warning", Severity::Warning),
            ]),
        }
    }

    fn msvc_link_spec() -> DiagnosticSpec {
        DiagnosticSpec {
            regex: MSVC_LINK_RE.to_string(),
            severity_map: sev(&[
                ("fatal error", Severity::Fatal),
                ("error", Severity::Error),
                ("warning", Severity::Warning),
            ]),
        }
    }

    fn watcom_cc_spec() -> DiagnosticSpec {
        DiagnosticSpec {
            regex: WATCOM_CC_RE.to_string(),
            severity_map: sev(&[("Error", Severity::Error), ("Warning", Severity::Warning)]),
        }
    }

    fn watcom_link_spec() -> DiagnosticSpec {
        DiagnosticSpec {
            regex: WATCOM_LINK_RE.to_string(),
            severity_map: BTreeMap::new(),
        }
    }

    /// rule-success.BuildDiagnosticRecorded, entity-fields.Diagnostic,
    /// enum-comparable.Severity: the `msvc_cc` dialect parses a real VC6
    /// compiler line into a fully-populated `Diagnostic` (no column).
    #[test]
    fn diagnostic_recorded_msvc_cc() {
        let stdout = "lexer.c(50): error C2065: 'tre': undeclared identifier\n";
        let diags = parse_diagnostics(&msvc_cc_spec(), stdout, "").unwrap();
        assert_eq!(diags.len(), 1);
        let d = &diags[0];
        assert_eq!(d.file.as_deref(), Some("lexer.c"));
        assert_eq!(d.line, Some(50));
        assert_eq!(d.column, None);
        assert_eq!(d.severity, Severity::Error);
        assert_eq!(d.code, "C2065");
        assert_eq!(d.message, "'tre': undeclared identifier");
    }

    /// entity-optional.Diagnostic.column: the modern `cl /diagnostics:column`
    /// form carries a column; the default form omits it (`None`, not `0`).
    #[test]
    fn diagnostic_column_optional() {
        let spec = msvc_cc_spec();

        let with_col = parse_diagnostics(&spec, "lexer.c(50,20): error C2065: x\n", "").unwrap();
        assert_eq!(with_col.len(), 1);
        assert_eq!(with_col[0].column, Some(20));
        assert_eq!(with_col[0].line, Some(50));

        let without_col = parse_diagnostics(&spec, "lexer.c(50): error C2065: x\n", "").unwrap();
        assert_eq!(without_col.len(), 1);
        assert_eq!(without_col[0].column, None);
        assert_eq!(without_col[0].line, Some(50));
    }

    /// entity-optional.Diagnostic.line: link-level errors carry no source
    /// position, so `line`/`column` are absent for both `msvc_link` forms.
    #[test]
    fn diagnostic_msvc_link() {
        let spec = msvc_link_spec();
        let stderr = "main.obj : error LNK2019: unresolved external symbol\n\
                      LINK : fatal error LNK1104: cannot open file 'x.lib'\n";
        let diags = parse_diagnostics(&spec, "", stderr).unwrap();
        assert_eq!(diags.len(), 2);

        assert_eq!(diags[0].line, None);
        assert_eq!(diags[0].column, None);
        assert_eq!(diags[0].file, None);
        assert_eq!(diags[0].severity, Severity::Error);
        assert_eq!(diags[0].code, "LNK2019");

        assert_eq!(diags[1].line, None);
        assert_eq!(diags[1].column, None);
        assert_eq!(diags[1].file, None);
        assert_eq!(diags[1].severity, Severity::Fatal);
        assert_eq!(diags[1].code, "LNK1104");
    }

    /// watcom_cc dialect: the exclamation-keyword compile form, line present,
    /// no column.
    #[test]
    fn diagnostic_watcom_cc() {
        let stdout = "err.c(9): Error! E1011: Symbol 'x' has not been declared\n";
        let diags = parse_diagnostics(&watcom_cc_spec(), stdout, "").unwrap();
        assert_eq!(diags.len(), 1);
        let d = &diags[0];
        assert_eq!(d.file.as_deref(), Some("err.c"));
        assert_eq!(d.line, Some(9));
        assert_eq!(d.column, None);
        assert_eq!(d.severity, Severity::Error);
        assert_eq!(d.code, "E1011");
        assert_eq!(d.message, "Symbol 'x' has not been declared");
    }

    /// watcom_link dialect: a bare 4-digit `wlink` code, no source position.
    /// With an empty `severity_map` and no `severity` capture, the severity
    /// falls back to `Info`.
    #[test]
    fn diagnostic_watcom_link() {
        let stdout = "1014 stack segment not found\n";
        let diags = parse_diagnostics(&watcom_link_spec(), stdout, "").unwrap();
        assert_eq!(diags.len(), 1);
        let d = &diags[0];
        assert_eq!(d.code, "1014");
        assert_eq!(d.line, None);
        assert_eq!(d.column, None);
        assert_eq!(d.file, None);
        assert_eq!(d.severity, Severity::Info);
        assert_eq!(d.message, "stack segment not found");
    }

    /// rule-failure.BuildResultProduced.1 (semantic pin): a nonzero compiler
    /// exit yields `success=false` but a fully-populated `BuildOutcome` with the
    /// parsed diagnostics — structured data, NOT an error path. A zero exit is
    /// `success=true`.
    #[test]
    fn compile_error_is_not_a_tool_error() {
        let spec = msvc_cc_spec();
        let stderr = "lexer.c(50): error C2065: 'tre': undeclared identifier\n";

        let failed = build_outcome(&spec, "msvc_cc", 2, "", stderr).unwrap();
        assert!(!failed.success);
        assert_eq!(failed.exit_code, 2);
        assert_eq!(failed.dialect, "msvc_cc");
        assert_eq!(failed.diagnostics.len(), 1);
        assert_eq!(failed.diagnostics[0].code, "C2065");
        assert_eq!(failed.diagnostics[0].severity, Severity::Error);

        let ok = build_outcome(&spec, "msvc_cc", 0, "", "").unwrap();
        assert!(ok.success);
        assert_eq!(ok.exit_code, 0);
        assert!(ok.diagnostics.is_empty());
    }

    /// SAFETY PIN diagnostic_regex_is_bounded: `BoundedRegex::compile` succeeds
    /// on a normal pattern and the linear-time engine parses adversarial input
    /// (a nested-quantifier pattern against a 100k-`a` line) without hanging —
    /// it simply returns. A pattern whose compiled program exceeds SIZE_LIMIT is
    /// rejected with `Err`, proving the size bound is wired.
    #[test]
    fn diagnostic_regex_is_bounded() {
        /* The nested-quantifier pattern that would catastrophically backtrack a
        backtracking engine; the regex crate is linear-time, so it cannot hang. */
        let nested = BoundedRegex::compile(r"^(a+)+$").unwrap();
        let adversarial = "a".repeat(100_000);
        /* No timeout needed: a return at all is the proof — a backtracking
        engine would not finish this in any reasonable time. */
        let matched = nested.as_regex().is_match(&adversarial);
        assert!(matched, "100k 'a's match ^(a+)+$ on the linear-time engine");

        /* A deliberately huge compiled program: a long run of bounded
        repetitions multiplies NFA states past the 64 KiB ceiling, so the build
        is rejected rather than run. */
        let huge = "(?:a{0,1000})".repeat(200);
        let err = BoundedRegex::compile(&huge);
        assert!(
            err.is_err(),
            "a pattern exceeding SIZE_LIMIT must be rejected, not compiled"
        );
    }
}
