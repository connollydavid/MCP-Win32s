//! Shared test harness: a mock device speaking the frozen wire protocol,
//! so the bridge's round-trip and mapping are tested without hardware.
//! Models the device side of `specs/wire-contract.allium`.

use anyhow::{bail, Result};
use async_trait::async_trait;
use mcp_w32s_bridge::device::Device;
use mcp_w32s_bridge::wire::{Command, Response};
use serde_json::json;

/// Scripts the device's echo behaviour: `BOOM` returns a status:"error"
/// reply (recoverable), anything else echoes the line back as `data`.
pub struct MockDevice;

#[async_trait]
impl Device for MockDevice {
    async fn call(&mut self, cmd: &Command) -> Result<Response> {
        let line = cmd.args.get("line").and_then(|v| v.as_str()).unwrap_or("");
        let reply = if line == "BOOM" {
            json!({"id": cmd.id, "status": "error", "error": "unknown command"})
        } else {
            json!({"id": cmd.id, "status": "ok", "data": line})
        };
        Ok(serde_json::from_value(reply)?)
    }
}

/// A device whose link is down — every call is a transport error.
pub struct DeadDevice;

#[async_trait]
impl Device for DeadDevice {
    async fn call(&mut self, _cmd: &Command) -> Result<Response> {
        bail!("link down")
    }
}
