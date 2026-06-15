//! An append-only, size-rotating structured audit log for power/destructive
//! tool calls. Power granted to the agent is logged, never silent
//! (`PowerToolsAreAudited`): every destructive/power tool call emits one JSON
//! line `{timestamp, tool, args_summary, outcome}` on EVERY outcome path
//! (relayed, refused locally, errored at the device). Read-only tools are not
//! audited.
//!
//! The sink is a file path (from `--audit-log`) with size-based rotation, or
//! stderr (the default). Hand-rolled with `std` + serde_json (no new deps):
//! the timestamp is whole seconds since the Unix epoch (no float), the
//! args_summary is redacted and truncated, and rotation renames the current
//! file to `<path>.1` when it would exceed the cap.

use serde_json::{json, Value};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

/// The recoverable outcome of a power-tool call — the `RecoverableOutcome`
/// partition plus `device_error`, written to the audit record so the log
/// distinguishes a local refusal from a device-side failure.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Relayed,
    DeviceCircuitOpen,
    RateLimited,
    UnsafeOptInAbsent,
    DeviceError,
}

impl Outcome {
    /// The serialized token (the wire/log form).
    pub fn as_str(self) -> &'static str {
        match self {
            Outcome::Relayed => "relayed",
            Outcome::DeviceCircuitOpen => "device_circuit_open",
            Outcome::RateLimited => "rate_limited",
            Outcome::UnsafeOptInAbsent => "unsafe_opt_in_absent",
            Outcome::DeviceError => "device_error",
        }
    }
}

/// The longest an args_summary string value may be before it is truncated (with
/// an explicit marker) — keeps a large base64 blob or argv out of the log.
const MAX_SUMMARY_VALUE: usize = 256;

/// Redact and truncate the relayed wire args into an audit-safe summary: every
/// string value is capped at `MAX_SUMMARY_VALUE` bytes (on a char boundary)
/// with a truncation marker; arrays/objects keep their structure with the same
/// per-string cap; everything else passes through. Nothing here is secret on
/// this wire, but a long base64 payload is noise, so it is summarised.
pub fn summarise_args(args: &serde_json::Map<String, Value>) -> Value {
    let mut out = serde_json::Map::new();
    for (k, v) in args {
        out.insert(k.clone(), redact_value(v));
    }
    Value::Object(out)
}

fn redact_value(v: &Value) -> Value {
    match v {
        Value::String(s) => Value::String(truncate(s)),
        Value::Array(items) => Value::Array(items.iter().map(redact_value).collect()),
        Value::Object(map) => {
            let mut out = serde_json::Map::new();
            for (k, val) in map {
                out.insert(k.clone(), redact_value(val));
            }
            Value::Object(out)
        }
        other => other.clone(),
    }
}

fn truncate(s: &str) -> String {
    if s.len() <= MAX_SUMMARY_VALUE {
        return s.to_string();
    }
    let mut end = MAX_SUMMARY_VALUE;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}…[{} bytes]", &s[..end], s.len())
}

/// Whole seconds since the Unix epoch (integer-only; no float). A clock before
/// this point records 0.
fn unix_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// Where audit lines go.
enum Sink {
    Stderr,
    /// A file path with size-based rotation at `max_bytes`.
    File {
        path: PathBuf,
        max_bytes: u64,
    },
}

/// The audit log. Each `record` serialises one JSON line and appends it to the
/// sink; a file sink rotates when it would exceed its size cap.
pub struct AuditLog {
    sink: Sink,
}

impl AuditLog {
    /// Audit to stderr (the default sink).
    pub fn stderr() -> Self {
        AuditLog { sink: Sink::Stderr }
    }

    /// Audit to a file at `path`, rotating to `<path>.1` when a write would
    /// push it past `max_bytes`.
    pub fn file(path: impl Into<PathBuf>, max_bytes: u64) -> Self {
        AuditLog {
            sink: Sink::File {
                path: path.into(),
                max_bytes: max_bytes.max(1),
            },
        }
    }

    /// The production file sink: 8 MiB before rotation.
    pub fn file_default(path: impl Into<PathBuf>) -> Self {
        AuditLog::file(path, 8 * 1024 * 1024)
    }

    /// Build the JSON line for a power-tool call. Public so a unit test can pin
    /// the record shape without touching a sink.
    pub fn line(tool: &str, args: &serde_json::Map<String, Value>, outcome: Outcome) -> String {
        let record = json!({
            "timestamp": unix_seconds(),
            "tool": tool,
            "args_summary": summarise_args(args),
            "outcome": outcome.as_str(),
        });
        record.to_string()
    }

    /// Append one audit record for a power-tool call. Best-effort and durable:
    /// an I/O failure is logged to stderr but never fails the tool call (the
    /// audit must not change the result the model sees).
    pub fn record(&self, tool: &str, args: &serde_json::Map<String, Value>, outcome: Outcome) {
        let line = AuditLog::line(tool, args, outcome);
        if let Err(e) = self.write_line(&line) {
            eprintln!("audit: failed to write record: {e}");
        }
    }

    fn write_line(&self, line: &str) -> io::Result<()> {
        match &self.sink {
            Sink::Stderr => {
                let mut err = io::stderr();
                err.write_all(line.as_bytes())?;
                err.write_all(b"\n")?;
                err.flush()
            }
            Sink::File { path, max_bytes } => {
                let needed = line.len() as u64 + 1;
                let existing = fs::metadata(path).map(|m| m.len()).unwrap_or(0);
                if existing > 0 && existing + needed > *max_bytes {
                    // Rotate: the current file becomes <path>.1 (overwriting an
                    // older rotation), then a fresh file is started.
                    let rotated = rotated_path(path);
                    let _ = fs::rename(path, &rotated);
                }
                let mut f: File = OpenOptions::new().create(true).append(true).open(path)?;
                f.write_all(line.as_bytes())?;
                f.write_all(b"\n")?;
                f.flush()
            }
        }
    }
}

/// `<path>.1` — the single rotation slot.
fn rotated_path(path: &std::path::Path) -> PathBuf {
    let mut s = path.as_os_str().to_os_string();
    s.push(".1");
    PathBuf::from(s)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Map;

    fn args(pairs: &[(&str, Value)]) -> Map<String, Value> {
        pairs
            .iter()
            .map(|(k, v)| (k.to_string(), v.clone()))
            .collect()
    }

    #[test]
    fn record_carries_tool_outcome_and_args() {
        let a = args(&[("argv", json!(["dir", "C:\\"])), ("unsafe", json!(true))]);
        let line = AuditLog::line("win32_exec", &a, Outcome::Relayed);
        let v: Value = serde_json::from_str(&line).expect("a valid JSON line");
        assert_eq!(v.get("tool").and_then(Value::as_str), Some("win32_exec"));
        assert_eq!(v.get("outcome").and_then(Value::as_str), Some("relayed"));
        assert!(v.get("timestamp").and_then(Value::as_u64).is_some());
        let summary = v.get("args_summary").expect("args_summary present");
        assert_eq!(summary.get("unsafe").and_then(Value::as_bool), Some(true));
        assert!(summary.get("argv").and_then(Value::as_array).is_some());
    }

    #[test]
    fn outcome_tokens_are_the_partition_plus_device_error() {
        assert_eq!(Outcome::Relayed.as_str(), "relayed");
        assert_eq!(Outcome::DeviceCircuitOpen.as_str(), "device_circuit_open");
        assert_eq!(Outcome::RateLimited.as_str(), "rate_limited");
        assert_eq!(Outcome::UnsafeOptInAbsent.as_str(), "unsafe_opt_in_absent");
        assert_eq!(Outcome::DeviceError.as_str(), "device_error");
    }

    #[test]
    fn long_string_values_are_truncated() {
        let big = "A".repeat(MAX_SUMMARY_VALUE + 100);
        let a = args(&[("data", json!(big))]);
        let line = AuditLog::line("win32_write_file", &a, Outcome::Relayed);
        let v: Value = serde_json::from_str(&line).unwrap();
        let data = v
            .pointer("/args_summary/data")
            .and_then(Value::as_str)
            .unwrap();
        assert!(
            data.len() < MAX_SUMMARY_VALUE + 100,
            "the value was truncated"
        );
        assert!(data.contains("bytes]"), "the truncation marker is present");
    }

    #[test]
    fn file_sink_writes_and_rotates_on_size() {
        let dir = std::env::temp_dir().join(format!("bridge-audit-{}", std::process::id()));
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("audit.log");
        let _ = fs::remove_file(&path);
        let _ = fs::remove_file(rotated_path(&path));

        // A tiny cap so the second record forces a rotation.
        let log = AuditLog::file(&path, 80);
        let a = args(&[("argv", json!(["dir"]))]);
        log.record("win32_exec", &a, Outcome::Relayed);
        let first_len = fs::metadata(&path).unwrap().len();
        assert!(first_len > 0, "the first record was written");

        // The next write exceeds 80 bytes total -> rotate, then write fresh.
        log.record("win32_exec", &a, Outcome::DeviceError);
        assert!(
            rotated_path(&path).exists(),
            "the previous file rotated to <path>.1"
        );
        let current = fs::read_to_string(&path).unwrap();
        assert_eq!(
            current.lines().count(),
            1,
            "the new file holds exactly the post-rotation record"
        );

        let _ = fs::remove_dir_all(&dir);
    }
}
