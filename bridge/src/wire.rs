//! The frozen MCP-Win32s wire protocol (newline-delimited JSON over
//! serial/TCP): the ready handshake and the generic command/response.
//! Mirrors `specs/wire-contract.allium` (the device side). The bridge
//! consumes this; it does not change it.

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::collections::BTreeMap;

/// The device's ready message — the first line on every connection,
/// before any command. Unknown keys are tolerated (the feature set
/// grows with capability uplift).
#[derive(Debug, Clone, Deserialize)]
pub struct Ready {
    pub status: String,
    pub codepage: i64,
    pub version: String,
    pub transport: String,
    pub features: Features,
    #[serde(default)]
    pub warning: Option<String>,
}

impl Ready {
    pub fn is_ready(&self) -> bool {
        self.status == "ready"
    }
}

/// One build toolchain the device detected at startup, carried in the ready
/// message's `features.toolchains` array (`specs/wire-contract.allium`
/// ReadyShape; `specs/toolchains.allium` DetectedToolchain). `version` is the
/// full banner string (e.g. `12.00.8804`) — the build number distinguishes
/// service packs the support matrix keys on.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct DetectedToolchain {
    pub vendor: String,
    pub command: String,
    pub version: String,
}

/// The device feature flags. The documented booleans are named; any
/// further keys (binary_classify, process_mitigation, and the future
/// `mem`/`encoding` tier strings) are captured in `extra`.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct Features {
    #[serde(default)]
    pub is_win32s: bool,
    #[serde(default)]
    pub is_win9x: bool,
    #[serde(default)]
    pub is_nt: bool,
    #[serde(default)]
    pub is_wow64: bool,
    #[serde(default)]
    pub threads: bool,
    #[serde(default)]
    pub job_objects: bool,
    #[serde(default)]
    pub ctrl_events: bool,
    #[serde(default)]
    pub pty: bool,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

/// A command to the device: `{"cmd":..,"id":.., <args>}`. The bridge issues
/// only `echo`; the typed capability commands are added later.
#[derive(Debug, Clone, Serialize)]
pub struct Command {
    pub cmd: String,
    pub id: String,
    #[serde(flatten)]
    pub args: Map<String, Value>,
}

impl Command {
    pub fn new(cmd: &str, id: String) -> Self {
        Command {
            cmd: cmd.to_string(),
            id,
            args: Map::new(),
        }
    }

    pub fn with(mut self, key: &str, value: Value) -> Self {
        self.args.insert(key.to_string(), value);
        self
    }
}

/// A device response: `{"id":..,"status":"ok|error", <fields>}`.
/// `error` carries the recoverable reason on a `status:"error"` reply.
#[derive(Debug, Clone, Deserialize)]
pub struct Response {
    #[serde(default)]
    pub id: String,
    pub status: String,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(flatten)]
    pub fields: Map<String, Value>,
}

impl Response {
    pub fn is_ok(&self) -> bool {
        self.status == "ok"
    }
}
