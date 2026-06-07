//! Device capabilities resolved from the ready message, and the
//! capability-gating contract (`specs/mcp-bridge.allium`): a tool that
//! names a required capability is advertised iff the device provides it.
//! The concrete capability tools arrive in 5.1-5.4; 5.0 fixes the gate.

use crate::wire::{DetectedToolchain, Features};

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
    /// Build toolchains the device detected at startup (the ready message's
    /// `features.toolchains` array). The bridge generates one
    /// `win32_<name>_<role>` tool per supported (definition, role) — see
    /// `server::Bridge::new`.
    pub toolchains: Vec<DetectedToolchain>,
    /// Operator opt-in for runtime `win32_register_toolchain` (the hybrid
    /// authoring model's dangerous half). This is a BRIDGE-operator flag, not a
    /// device wire field — registration exposes a new tool surface, so the
    /// operator running the bridge consents to it, not the device. Off by
    /// default (`RegistrationRequiresOptIn`).
    pub toolchain_registration: bool,
    /// Operator opt-in for memory WRITES (the `win32_poke` tool). A
    /// BRIDGE-operator flag (`--allow-memory-write`), not a device wire field.
    /// It gates `win32_poke` ADVERTISEMENT via the two-factor `mem_write`
    /// capability (`mem != None && allow_memory_write`) — the bridge half of
    /// `PokeRequiresBothArmingLayers`; the device `/ALLOWMEMWRITE` arm is the
    /// independent device half. Off by default (`MemoryWriteToolRequiresOptIn`).
    pub allow_memory_write: bool,
}

impl Capabilities {
    /// Resolve from the ready message. `mem`/`encoding` tier strings land
    /// with the 5.3/5.4 device work; until then they default
    /// conservatively (no memory access, codepage encoding).
    /// `allow_registration` is the bridge-operator opt-in (a CLI flag), not a
    /// device-reported capability.
    pub fn from_ready(
        codepage: i64,
        version: String,
        f: &Features,
        allow_registration: bool,
        allow_memory_write: bool,
    ) -> Self {
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
        // `features.toolchains` is an unknown key, so it rides `extra`. A
        // malformed entry is dropped (an empty array, not a hard failure).
        let toolchains = f
            .extra
            .get("toolchains")
            .and_then(|v| serde_json::from_value::<Vec<DetectedToolchain>>(v.clone()).ok())
            .unwrap_or_default();
        Capabilities {
            has_pty: f.pty,
            mem,
            encoding,
            codepage,
            version,
            toolchains,
            toolchain_registration: allow_registration,
            allow_memory_write,
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
            // The TWO-FACTOR poke gate (MemoryWriteToolRequiresOptIn): a memory
            // tier AND the operator opt-in. The bridge half of
            // PokeRequiresBothArmingLayers (the device /ALLOWMEMWRITE arm is the
            // independent device half).
            "mem_write" => self.mem != MemTier::None && self.allow_memory_write,
            "utf8" => self.encoding == EncodingMode::Utf8Native,
            // The runtime-registration opt-in gates win32_register_toolchain
            // (RegistrationRequiresOptIn).
            "toolchain_registration" => self.toolchain_registration,
            _ => false,
        }
    }
}

/// The capability a tool requires to be advertised. `None` = always
/// advertised; `Some(cap)` = advertised iff `Capabilities::satisfies(cap)`.
/// 5.3 wires the FIRST real entries (the gate was empty/unexercised since
/// 5.0): the five memory tools. The four read/control tools require `mem`
/// (any tier); `win32_poke` requires the two-factor `mem_write`
/// (`MemoryWriteToolRequiresOptIn`). On a `mem: none` device all five are
/// pruned from `tools/list` — the first real exercise of the G1
/// prune→absence path.
pub const GATED_TOOLS: &[(&str, &str)] = &[
    ("win32_spawn_retain", "mem"),
    ("win32_peek", "mem"),
    ("win32_poke", "mem_write"),
    ("win32_terminate", "mem"),
    ("win32_release", "mem"),
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
