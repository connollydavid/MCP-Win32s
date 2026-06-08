//! 5.3 integration tests: the memory peek/poke tool surface — the five
//! win32_* memory tools, the FIRST real G1 capability prune (mem: none ->
//! absent from tools/list), the two-factor poke opt-in gate, honest hints,
//! and the 1:1 device-command relay. Each cites its obligation IDs from
//! tests/OBLIGATIONS-5.3.md.

mod common;
use common as mock;

use async_trait::async_trait;
use mcp_w32s_bridge::capabilities::{Capabilities, EncodingProvenance, MemTier};
use mcp_w32s_bridge::device::Device;
use mcp_w32s_bridge::server::Bridge;
use mcp_w32s_bridge::wire::{Command, Response};
use rmcp::model::{CallToolRequestParams, Tool};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{json, Value};
use std::sync::{Arc, Mutex};

/// The five memory tools, in registration order.
const MEM_TOOLS: &[&str] = &[
    "win32_spawn_retain",
    "win32_peek",
    "win32_poke",
    "win32_terminate",
    "win32_release",
];

fn caps(mem: MemTier, allow_memory_write: bool) -> Capabilities {
    Capabilities {
        has_pty: false,
        mem,
        encoding: EncodingProvenance::FromCodepage,
        codepage: 437,
        version: "test".to_string(),
        toolchains: vec![],
        toolchain_registration: false,
        allow_memory_write,
        allow_unsafe_exec: false,
    }
}

async fn serve(caps: Capabilities, device: Box<dyn Device>) -> RunningService<RoleClient, ()> {
    let (server_t, client_t) = tokio::io::duplex(16384);
    let bridge = Bridge::new(caps, device);
    tokio::spawn(async move {
        if let Ok(server) = bridge.serve(server_t).await {
            let _ = server.waiting().await;
        }
    });
    ().serve(client_t).await.expect("client up")
}

async fn list_tools(client: &RunningService<RoleClient, ()>) -> Vec<Tool> {
    client
        .list_tools(Default::default())
        .await
        .expect("list_tools")
        .tools
}

fn find<'a>(tools: &'a [Tool], name: &str) -> Option<&'a Tool> {
    tools.iter().find(|t| t.name == name)
}

/// One command the mock saw: the verb and its serialized argument map.
type Seen = (String, serde_json::Map<String, Value>);

/// A mock device that records each command and answers the memory verbs with
/// a tier-shaped ok reply, so a test can both inspect the relayed wire args
/// and confirm the device fields ride structuredContent. `peek` returns a
/// `data_b64`; the others return a small ack.
#[derive(Clone)]
struct MemDevice {
    received: Arc<Mutex<Vec<Seen>>>,
}

impl MemDevice {
    fn new() -> Self {
        MemDevice {
            received: Arc::new(Mutex::new(Vec::new())),
        }
    }
    fn last(&self) -> Seen {
        self.received
            .lock()
            .unwrap()
            .last()
            .cloned()
            .expect("a command was sent")
    }
}

#[async_trait]
impl Device for MemDevice {
    async fn call(&mut self, cmd: &Command) -> Result<Response, anyhow::Error> {
        self.received
            .lock()
            .unwrap()
            .push((cmd.cmd.clone(), cmd.args.clone()));
        let reply = match cmd.cmd.as_str() {
            "spawnRetain" => json!({"id": cmd.id, "status": "ok", "token": "m1-742", "pid": 4242}),
            "peek" => {
                json!({"id": cmd.id, "status": "ok", "data_b64": "QUJD", "bytes_read": 3, "truncated": false})
            }
            "poke" => json!({"id": cmd.id, "status": "ok", "bytes_written": 3, "partial": false}),
            "terminate" => json!({"id": cmd.id, "status": "ok", "terminated": true}),
            "release" => json!({"id": cmd.id, "status": "ok", "released": true}),
            _ => json!({"id": cmd.id, "status": "error", "error": "unknown command"}),
        };
        Ok(serde_json::from_value(reply)?)
    }
}

/// rule-success.MemoryToolsRegistered, rule-entity-creation.MemoryToolsRegistered.1
/// — on a memory-capable device (with the write opt-in so poke shows too) all
/// five memory tools are advertised, with object inputSchemas.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn memory_tools_registered() {
    let client = serve(caps(MemTier::Process, true), Box::new(MemDevice::new())).await;
    let tools = list_tools(&client).await;
    for name in MEM_TOOLS {
        let t = find(&tools, name).unwrap_or_else(|| panic!("{name} is advertised"));
        assert_eq!(
            t.input_schema.get("type").and_then(Value::as_str),
            Some("object"),
            "{name} has an object inputSchema"
        );
        assert_eq!(
            t.input_schema.get("additionalProperties"),
            Some(&Value::Bool(false)),
            "{name} inputSchema is closed"
        );
    }
    let _ = client.cancel().await;
}

/// The FIRST real G1 capability prune (carried empty/unexercised since 5.0):
/// with `mem: none` the four mem-gated tools are ABSENT from tools/list; with
/// `mem: process` they appear. The prune->absence path, finally exercised.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn peek_pruned_when_mem_none() {
    // mem: none -> all five pruned.
    let none = serve(caps(MemTier::None, true), Box::new(MemDevice::new())).await;
    let tools = list_tools(&none).await;
    for name in MEM_TOOLS {
        assert!(
            find(&tools, name).is_none(),
            "{name} is pruned on a mem:none device"
        );
    }
    let _ = none.cancel().await;

    // mem: process -> the four mem-gated tools appear (poke needs the write
    // opt-in, tested separately).
    let proc = serve(caps(MemTier::Process, false), Box::new(MemDevice::new())).await;
    let tools = list_tools(&proc).await;
    for name in [
        "win32_spawn_retain",
        "win32_peek",
        "win32_terminate",
        "win32_release",
    ] {
        assert!(
            find(&tools, name).is_some(),
            "{name} is advertised on a memory-capable device"
        );
    }
    let _ = proc.cancel().await;
}

/// invariant.MemoryWriteToolRequiresOptIn (SAFETY PIN #7, bridge half) —
/// win32_poke is advertised only when mem != none AND the operator passed
/// --allow-memory-write (the two-factor mem_write gate). Absent without the
/// opt-in even on a memory-capable device, and absent without a tier even
/// with the opt-in.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn poke_requires_opt_in() {
    // Memory-capable but NOT opted in -> poke absent (peek still present).
    let no_optin = serve(caps(MemTier::Process, false), Box::new(MemDevice::new())).await;
    let tools = list_tools(&no_optin).await;
    assert!(
        find(&tools, "win32_poke").is_none(),
        "poke absent without opt-in"
    );
    assert!(
        find(&tools, "win32_peek").is_some(),
        "peek present without opt-in"
    );
    let _ = no_optin.cancel().await;

    // Opted in AND memory-capable -> poke advertised.
    let armed = serve(caps(MemTier::Process, true), Box::new(MemDevice::new())).await;
    assert!(
        find(&list_tools(&armed).await, "win32_poke").is_some(),
        "poke advertised with tier + opt-in"
    );
    let _ = armed.cancel().await;

    // Opt-in but NO tier -> poke still absent (needs both factors).
    let no_tier = serve(caps(MemTier::None, true), Box::new(MemDevice::new())).await;
    assert!(
        find(&list_tools(&no_tier).await, "win32_poke").is_none(),
        "poke absent without a memory tier even when opted in"
    );
    let _ = no_tier.cancel().await;
}

/// invariant.MemoryToolHintsAreHonest — spawn_retain/poke/terminate carry
/// destructive; peek carries read_only; read_only excludes destructive. (Hint
/// absence is read as false, matching the file-tools convention.)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn memory_tool_hints_honest() {
    let client = serve(caps(MemTier::Process, true), Box::new(MemDevice::new())).await;
    let tools = list_tools(&client).await;

    let read_only = |name: &str| -> bool {
        find(&tools, name)
            .and_then(|t| t.annotations.as_ref())
            .and_then(|a| a.read_only_hint)
            .unwrap_or(false)
    };
    let destructive = |name: &str| -> bool {
        find(&tools, name)
            .and_then(|t| t.annotations.as_ref())
            .and_then(|a| a.destructive_hint)
            .unwrap_or(false)
    };

    for name in ["win32_spawn_retain", "win32_poke", "win32_terminate"] {
        assert!(destructive(name), "{name} is destructive");
        assert!(!read_only(name), "{name} is not read-only");
    }
    assert!(read_only("win32_peek"), "peek is read-only");
    assert!(!destructive("win32_peek"), "peek is not destructive");
    // release relinquishes a handle without destroying state: neither hint.
    assert!(!destructive("win32_release"), "release is not destructive");
    assert!(!read_only("win32_release"), "release is not read-only");
    // No memory tool is ever both read-only and destructive.
    for name in MEM_TOOLS {
        assert!(
            !(read_only(name) && destructive(name)),
            "{name} is not both read-only and destructive"
        );
    }
    let _ = client.cancel().await;
}

/// rule-success.MemoryToolsRegistered (relay mapping) — each tool round-trips
/// to its device wire command with name-mapped args (argv/cwd; token->mem_token,
/// addr->mem_addr, len->mem_len as STRINGS, data->data). A device ok reply rides
/// structuredContent (peek's data_b64); a device status:error -> recoverable
/// isError.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn memory_tools_relay() {
    let dev = MemDevice::new();
    let client = serve(caps(MemTier::Process, true), Box::new(dev.clone())).await;

    // spawn_retain: argv + cwd -> the spawnRetain wire command.
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_spawn_retain").with_arguments(
                json!({"argv": ["cl", "/c", "x.c"], "cwd": "C:\\PROJ"})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("spawn call ok");
    assert_eq!(res.is_error, Some(false), "spawn ok is not an error");
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "spawnRetain");
    assert_eq!(
        args.get("argv").and_then(Value::as_array).map(|a| a.len()),
        Some(3),
        "argv relayed as an array"
    );
    assert_eq!(args.get("cwd").and_then(Value::as_str), Some("C:\\PROJ"));
    // The device token rides structuredContent.
    let sc = res.structured_content.expect("spawn is structured");
    assert_eq!(sc.get("token").and_then(Value::as_str), Some("m1-742"));

    // peek: token/addr/len -> mem_token/mem_addr/mem_len, the last two STRINGS.
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_peek").with_arguments(
                json!({"token": "m1-742", "addr": "0x401000", "len": 3})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("peek call ok");
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "peek");
    assert_eq!(
        args.get("mem_token").and_then(Value::as_str),
        Some("m1-742")
    );
    assert_eq!(
        args.get("mem_addr").and_then(Value::as_str),
        Some("0x401000")
    );
    assert_eq!(
        args.get("mem_len").and_then(Value::as_str),
        Some("3"),
        "length is relayed as a STRING (never the int path)"
    );
    let sc = res.structured_content.expect("peek is structured");
    assert_eq!(sc.get("data_b64").and_then(Value::as_str), Some("QUJD"));

    // poke: token/addr/data -> mem_token/mem_addr/data.
    client
        .call_tool(
            CallToolRequestParams::new("win32_poke").with_arguments(
                json!({"token": "m1-742", "addr": "0x401000", "data": "QUJD"})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("poke call ok");
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "poke");
    assert_eq!(
        args.get("mem_token").and_then(Value::as_str),
        Some("m1-742")
    );
    assert_eq!(
        args.get("mem_addr").and_then(Value::as_str),
        Some("0x401000")
    );
    assert_eq!(args.get("data").and_then(Value::as_str), Some("QUJD"));

    // terminate + release: token -> mem_token.
    for (tool, verb) in [
        ("win32_terminate", "terminate"),
        ("win32_release", "release"),
    ] {
        client
            .call_tool(
                CallToolRequestParams::new(tool)
                    .with_arguments(json!({"token": "m1-742"}).as_object().unwrap().clone()),
            )
            .await
            .expect("token call ok");
        let (cmd, args) = dev.last();
        assert_eq!(cmd, verb, "{tool} relays to {verb}");
        assert_eq!(
            args.get("mem_token").and_then(Value::as_str),
            Some("m1-742")
        );
    }
    let _ = client.cancel().await;
}

/// A device status:error on a memory verb is a recoverable isError tool result
/// (bad/released token, unarmed, guard reject, audit-fail) — not a panic.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn memory_device_error_is_iserror() {
    let client = serve(
        caps(MemTier::Process, true),
        Box::new(mock::RecordingDevice::error("invalid token")),
    )
    .await;
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_peek").with_arguments(
                json!({"token": "bogus", "addr": "0x1000", "len": 8})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("call returns with a tool error");
    assert_eq!(
        res.is_error,
        Some(true),
        "a device error is a recoverable isError"
    );
    let _ = client.cancel().await;
}
