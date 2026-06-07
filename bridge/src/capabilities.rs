//! Device capabilities resolved from the ready message, and the
//! capability-gating contract (`specs/mcp-bridge.allium`): a tool that
//! names a required capability is advertised iff the device provides it.
//! The concrete capability tools arrive in 5.1-5.4; 5.0 fixes the gate.

use crate::wire::Features;

/// How far the device permits memory access (the Capabilities tier from
/// its ready message; the peek/poke tools arrive in 5.3).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemTier {
    None,
    Process,
    Arena,
    SharedVm,
}

/// The device's active text encoding for its own (path) surfaces.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EncodingMode {
    Codepage,
    Utf8Native,
}

/// The bridge's view of what the device supports, read once from the
/// ready message and immutable for the connection.
#[derive(Debug, Clone)]
pub struct Capabilities {
    pub has_pty: bool,
    pub mem: MemTier,
    pub encoding: EncodingMode,
    pub codepage: i64,
    pub version: String,
}

impl Capabilities {
    /// Resolve from the ready message. `mem`/`encoding` tier strings land
    /// with the 5.3/5.4 device work; until then they default
    /// conservatively (no memory access, codepage encoding).
    pub fn from_ready(codepage: i64, version: String, f: &Features) -> Self {
        let mem = match f.extra.get("mem").and_then(|v| v.as_str()) {
            Some("process") => MemTier::Process,
            Some("arena") => MemTier::Arena,
            Some("shared_vm") => MemTier::SharedVm,
            _ => MemTier::None,
        };
        let encoding = match f.extra.get("encoding").and_then(|v| v.as_str()) {
            Some("utf8_native") => EncodingMode::Utf8Native,
            _ => EncodingMode::Codepage,
        };
        Capabilities {
            has_pty: f.pty,
            mem,
            encoding,
            codepage,
            version,
        }
    }

    /// The spec's `capability_satisfied` black box: does the device
    /// provide the named capability? An unknown capability is treated as
    /// unsatisfied (conservative — an unrecognised gate hides its tool
    /// rather than exposing it).
    pub fn satisfies(&self, cap: &str) -> bool {
        match cap {
            "pty" => self.has_pty,
            "mem" => self.mem != MemTier::None,
            "utf8" => self.encoding == EncodingMode::Utf8Native,
            _ => false,
        }
    }
}

/// The capability a tool requires to be advertised. `None` = always
/// advertised; `Some(cap)` = advertised iff `Capabilities::satisfies(cap)`.
/// The 5.1-5.4 tools register their gate here; 5.0 ships an empty gate
/// set (only the ungated `win32_echo`).
pub const GATED_TOOLS: &[(&str, &str)] = &[
    // ("win32_pty_exec", "pty"),   // 5.3
    // ("win32_peek", "mem"),       // 5.3
    // ("win32_poke", "mem"),       // 5.3
];

/// The tool names that should be pruned from the router because the
/// device does not provide their required capability.
pub fn tools_to_prune(caps: &Capabilities) -> Vec<&'static str> {
    GATED_TOOLS
        .iter()
        .filter(|(_, cap)| !caps.satisfies(cap))
        .map(|(name, _)| *name)
        .collect()
}
