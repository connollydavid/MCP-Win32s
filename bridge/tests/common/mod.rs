//! Shared test harness: a mock device speaking the frozen wire protocol,
//! so the bridge's round-trip and mapping are tested without hardware.
//! Models the device side of `specs/wire-contract.allium`.
//!
//! This module is shared by several test crates; each uses a subset of the
//! mocks, so unused items per crate are expected.
#![allow(dead_code)]

use anyhow::{bail, Result};
use async_trait::async_trait;
use base64::Engine;
use mcp_w32s_bridge::device::Device;
use mcp_w32s_bridge::wire::{Command, Response};
use serde_json::json;
use std::sync::{Arc, Mutex};

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

/// One command as the mock device saw it on the wire: the `cmd` verb and
/// the serialized argument map (path/dest/data/…). Used to assert the
/// bridge relays each file tool 1:1 with name-mapped arguments.
#[derive(Debug, Clone)]
pub struct Received {
    pub cmd: String,
    pub args: serde_json::Map<String, serde_json::Value>,
}

/// A recording mock device: captures every command it receives and answers
/// with a scripted reply. `reply_error` makes every call a recoverable
/// `status:"error"`; otherwise it acks `status:"ok"` with a `message`.
/// The shared `received` log lets a test inspect what reached the device.
#[derive(Clone)]
pub struct RecordingDevice {
    pub received: Arc<Mutex<Vec<Received>>>,
    reply_error: Option<String>,
    ok_message: String,
}

impl RecordingDevice {
    /// A mock that acks every command with `{"status":"ok","message":<msg>}`.
    pub fn ok(message: &str) -> Self {
        RecordingDevice {
            received: Arc::new(Mutex::new(Vec::new())),
            reply_error: None,
            ok_message: message.to_string(),
        }
    }

    /// A mock that fails every command with `{"status":"error","error":<reason>}`.
    pub fn error(reason: &str) -> Self {
        RecordingDevice {
            received: Arc::new(Mutex::new(Vec::new())),
            reply_error: Some(reason.to_string()),
            ok_message: String::new(),
        }
    }
}

#[async_trait]
impl Device for RecordingDevice {
    async fn call(&mut self, cmd: &Command) -> Result<Response> {
        self.received.lock().unwrap().push(Received {
            cmd: cmd.cmd.clone(),
            args: cmd.args.clone(),
        });
        let reply = match &self.reply_error {
            Some(reason) => json!({"id": cmd.id, "status": "error", "error": reason}),
            None => json!({"id": cmd.id, "status": "ok", "message": self.ok_message}),
        };
        Ok(serde_json::from_value(reply)?)
    }
}

/// A mock device that answers `exec` with a scripted compiler result —
/// `{status:"ok", exit_code, stdout_b64, stderr_b64}`, the device exec reply
/// shape (`src/mcp-w32s.c`). The build tools are the bridge's first `exec`
/// user; this lets a test drive the full build pipeline (argv emission →
/// catalogued exec → base64 decode → diagnostic parse) and inspect the argv
/// that reached the wire. Any non-`exec` command replies a recoverable error.
#[derive(Clone)]
pub struct BuildDevice {
    pub received: Arc<Mutex<Vec<Received>>>,
    exit_code: i64,
    stdout: String,
    stderr: String,
}

impl BuildDevice {
    /// A mock whose `exec` returns the given exit code and output streams.
    pub fn new(exit_code: i64, stdout: &str, stderr: &str) -> Self {
        BuildDevice {
            received: Arc::new(Mutex::new(Vec::new())),
            exit_code,
            stdout: stdout.to_string(),
            stderr: stderr.to_string(),
        }
    }

    /// The argv of the single `exec` the bridge sent (panics if none yet).
    pub fn exec_argv(&self) -> Vec<String> {
        let log = self.received.lock().unwrap();
        let exec = log
            .iter()
            .find(|r| r.cmd == "exec")
            .expect("an exec command was sent");
        exec.args
            .get("argv")
            .and_then(|v| v.as_array())
            .expect("exec carries an argv array")
            .iter()
            .map(|v| v.as_str().unwrap_or_default().to_string())
            .collect()
    }
}

#[async_trait]
impl Device for BuildDevice {
    async fn call(&mut self, cmd: &Command) -> Result<Response> {
        self.received.lock().unwrap().push(Received {
            cmd: cmd.cmd.clone(),
            args: cmd.args.clone(),
        });
        let reply = if cmd.cmd == "exec" {
            let engine = base64::engine::general_purpose::STANDARD;
            json!({
                "id": cmd.id,
                "status": "ok",
                "exit_code": self.exit_code,
                "stdout_b64": engine.encode(self.stdout.as_bytes()),
                "stderr_b64": engine.encode(self.stderr.as_bytes()),
                "stdout_truncated": false,
                "stderr_truncated": false,
            })
        } else {
            json!({"id": cmd.id, "status": "error", "error": "unknown command"})
        };
        Ok(serde_json::from_value(reply)?)
    }
}
