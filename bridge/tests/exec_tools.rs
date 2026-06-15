//! Integration tests: the exec discovery tool surface — the three exec
//! tools (win32_exec / win32_pty_exec / win32_list_commands), the pty
//! capability prune (PtyToolGatedOnCapability), THE UNSAFE-EXEC SAFETY GATE
//! (UnsafeExecRequiresOperatorOptIn: refused without opt-in, relayed with it),
//! honest hints, and the 1:1 device-command relay. Each test cites its
//! obligation IDs from bridge/OBLIGATIONS-5.5.md.

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

/// caps with the two 5.5-relevant knobs: ConPTY support and the unsafe-exec
/// operator opt-in. Everything else is a conservative default.
fn caps(has_pty: bool, allow_unsafe_exec: bool) -> Capabilities {
    Capabilities {
        has_pty,
        mem: MemTier::None,
        encoding: EncodingProvenance::FromCodepage,
        codepage: 437,
        version: "test".to_string(),
        toolchains: vec![],
        toolchain_registration: false,
        allow_memory_write: false,
        allow_unsafe_exec,
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

/// A mock that records each exec/ptyExec/listCommands command and answers with a
/// shaped ok reply, so a test can inspect the relayed wire args AND confirm the
/// device receives nothing when the bridge refuses locally.
#[derive(Clone)]
struct ExecDevice {
    received: Arc<Mutex<Vec<Seen>>>,
}

impl ExecDevice {
    fn new() -> Self {
        ExecDevice {
            received: Arc::new(Mutex::new(Vec::new())),
        }
    }
    fn count(&self, verb: &str) -> usize {
        self.received
            .lock()
            .unwrap()
            .iter()
            .filter(|(c, _)| c == verb)
            .count()
    }
    fn last(&self) -> Seen {
        self.received
            .lock()
            .unwrap()
            .last()
            .cloned()
            .expect("a command was sent")
    }
    fn is_empty(&self) -> bool {
        self.received.lock().unwrap().is_empty()
    }
}

#[async_trait]
impl Device for ExecDevice {
    async fn call(&mut self, cmd: &Command) -> Result<Response, anyhow::Error> {
        self.received
            .lock()
            .unwrap()
            .push((cmd.cmd.clone(), cmd.args.clone()));
        let reply = match cmd.cmd.as_str() {
            "exec" | "ptyExec" => json!({
                "id": cmd.id, "status": "ok", "exit_code": 0,
                "stdout_b64": "", "stderr_b64": "",
                "stdout_truncated": false, "stderr_truncated": false
            }),
            "listCommands" => {
                json!({"id": cmd.id, "status": "ok", "commands": ["dir", "cl", "echo"]})
            }
            _ => json!({"id": cmd.id, "status": "error", "error": "unknown command"}),
        };
        Ok(serde_json::from_value(reply)?)
    }
}

fn read_only(tools: &[Tool], name: &str) -> bool {
    find(tools, name)
        .and_then(|t| t.annotations.as_ref())
        .and_then(|a| a.read_only_hint)
        .unwrap_or(false)
}

fn destructive(tools: &[Tool], name: &str) -> bool {
    find(tools, name)
        .and_then(|t| t.annotations.as_ref())
        .and_then(|a| a.destructive_hint)
        .unwrap_or(false)
}

/// rule-success.ExecToolsRegistered, rule-entity-creation.ExecToolsRegistered.1
/// — on a ready, pty-capable device tools/list shows the three exec tools with
/// the right hints and a 2020-12 object inputSchema. (device_cmd is asserted by
/// the relay test; the registration test pins presence + hints + schema.)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn exec_tools_registered() {
    let client = serve(caps(true, false), Box::new(ExecDevice::new())).await;
    let tools = list_tools(&client).await;
    for name in ["win32_exec", "win32_pty_exec", "win32_list_commands"] {
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
    // win32_exec / win32_pty_exec destructive; win32_list_commands read-only.
    assert!(destructive(&tools, "win32_exec"));
    assert!(destructive(&tools, "win32_pty_exec"));
    assert!(read_only(&tools, "win32_list_commands"));
    let _ = client.cancel().await;
}

/// invariant.PtyToolGatedOnCapability + the G1 prune — with has_pty:false,
/// win32_pty_exec is ABSENT; with has_pty:true it is present. win32_exec and
/// win32_list_commands (no required capability) are present in BOTH cases.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn pty_exec_pruned_when_no_pty() {
    // No ConPTY -> pty tool pruned, exec + list_commands still present.
    let no_pty = serve(caps(false, false), Box::new(ExecDevice::new())).await;
    let tools = list_tools(&no_pty).await;
    assert!(
        find(&tools, "win32_pty_exec").is_none(),
        "win32_pty_exec is pruned on a non-ConPTY device"
    );
    assert!(
        find(&tools, "win32_exec").is_some(),
        "exec present without pty"
    );
    assert!(
        find(&tools, "win32_list_commands").is_some(),
        "list_commands present without pty"
    );
    let _ = no_pty.cancel().await;

    // ConPTY -> the pty tool appears.
    let with_pty = serve(caps(true, false), Box::new(ExecDevice::new())).await;
    let tools = list_tools(&with_pty).await;
    assert!(
        find(&tools, "win32_pty_exec").is_some(),
        "win32_pty_exec advertised on a ConPTY device"
    );
    assert!(find(&tools, "win32_exec").is_some());
    assert!(find(&tools, "win32_list_commands").is_some());
    let _ = with_pty.cancel().await;
}

/// rule-success.ExecUnsafeRejected (+.1/.failure.{1,2,3}/creation.1) THE SAFETY
/// PIN — win32_exec with unsafe:true on a session whose allow_unsafe_exec:false
/// is a recoverable isError with the exact text, and the device receives
/// NOTHING (the unsafe request never relays).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn unsafe_exec_refused_without_opt_in() {
    let dev = ExecDevice::new();
    let client = serve(caps(false, false), Box::new(dev.clone())).await;
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_exec").with_arguments(
                json!({"argv": ["calc"], "unsafe": true})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("the call returns with a tool error");
    assert_eq!(
        res.is_error,
        Some(true),
        "unsafe exec without opt-in is a recoverable isError"
    );
    let text = res
        .content
        .iter()
        .find_map(|c| c.as_text().map(|t| t.text.clone()))
        .expect("an error text block");
    assert_eq!(text, "unsafe exec not permitted: operator opt-in required");
    assert!(
        res.structured_content.is_none(),
        "a refusal carries no structuredContent"
    );
    // THE DEVICE RECEIVED NOTHING.
    assert!(
        dev.is_empty(),
        "the unsafe request was NOT relayed to the device"
    );
    let _ = client.cancel().await;
}

/// rule-failure.ToolCallDispatched.{2,3} THE SAFETY PIN (dispatch half) —
/// win32_exec with unsafe:true on a session whose allow_unsafe_exec:true is
/// RELAYED, the device recording exactly one `exec` carrying `unsafe:true`; a
/// plain exec (no unsafe) is always relayed and carries no `unsafe`.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn unsafe_exec_relayed_with_opt_in() {
    let dev = ExecDevice::new();
    let client = serve(caps(false, true), Box::new(dev.clone())).await;

    // unsafe:true + opt-in -> relayed with unsafe on the wire.
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_exec").with_arguments(
                json!({"argv": ["calc"], "unsafe": true})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("exec call ok");
    assert_eq!(
        res.is_error,
        Some(false),
        "the relayed exec is not an error"
    );
    assert_eq!(dev.count("exec"), 1, "exactly one exec reached the device");
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "exec");
    assert_eq!(
        args.get("unsafe").and_then(Value::as_bool),
        Some(true),
        "the unsafe flag rides the wire under the opt-in"
    );
    assert_eq!(
        args.get("argv").and_then(Value::as_array).map(|a| a.len()),
        Some(1)
    );

    // A plain exec (unsafe omitted) is always relayed and carries no `unsafe`.
    client
        .call_tool(
            CallToolRequestParams::new("win32_exec")
                .with_arguments(json!({"argv": ["dir"]}).as_object().unwrap().clone()),
        )
        .await
        .expect("plain exec ok");
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "exec");
    assert!(
        args.get("unsafe").is_none(),
        "a plain exec carries no unsafe flag on the wire"
    );
    let _ = client.cancel().await;
}

/// invariant.ExecToolHintsAreHonest — exec/pty_exec are destructive and not
/// read_only; list_commands is read_only and not destructive; none is both.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn exec_tool_hints_honest() {
    let client = serve(caps(true, false), Box::new(ExecDevice::new())).await;
    let tools = list_tools(&client).await;
    for name in ["win32_exec", "win32_pty_exec"] {
        assert!(destructive(&tools, name), "{name} is destructive");
        assert!(!read_only(&tools, name), "{name} is not read-only");
    }
    assert!(
        read_only(&tools, "win32_list_commands"),
        "list_commands is read-only"
    );
    assert!(
        !destructive(&tools, "win32_list_commands"),
        "list_commands is not destructive"
    );
    for name in ["win32_exec", "win32_pty_exec", "win32_list_commands"] {
        assert!(
            !(read_only(&tools, name) && destructive(&tools, name)),
            "{name} is not both read-only and destructive"
        );
    }
    let _ = client.cancel().await;
}

/// invariant.ExecToolsAreDirectRelays — each exec tool round-trips to exactly
/// its device wire command with name-mapped args (exec/ptyExec/listCommands),
/// one call per tool. A device ok rides structuredContent.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn exec_tools_are_relays() {
    let dev = ExecDevice::new();
    let client = serve(caps(true, false), Box::new(dev.clone())).await;

    // win32_exec -> exec, with every named optional mapped onto the wire.
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_exec").with_arguments(
                json!({
                    "argv": ["cl", "/c", "x.c"], "cwd": "C:\\PROJ",
                    "timeout_ms": 5000, "max_output": 8192, "shell": false,
                    "mem_cap_bytes": 1048576, "cpu_time_ms": 3000,
                    "stdin_b64": "QQ=="
                })
                .as_object()
                .unwrap()
                .clone(),
            ),
        )
        .await
        .expect("exec ok");
    assert_eq!(res.is_error, Some(false));
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "exec");
    assert_eq!(
        args.get("argv").and_then(Value::as_array).map(|a| a.len()),
        Some(3)
    );
    assert_eq!(args.get("cwd").and_then(Value::as_str), Some("C:\\PROJ"));
    assert_eq!(args.get("timeout_ms").and_then(Value::as_i64), Some(5000));
    assert_eq!(args.get("max_output").and_then(Value::as_i64), Some(8192));
    assert_eq!(args.get("shell").and_then(Value::as_bool), Some(false));
    assert_eq!(
        args.get("mem_cap_bytes").and_then(Value::as_i64),
        Some(1048576)
    );
    assert_eq!(args.get("cpu_time_ms").and_then(Value::as_i64), Some(3000));
    assert_eq!(args.get("stdin_b64").and_then(Value::as_str), Some("QQ=="));
    let sc = res.structured_content.expect("exec ok is structured");
    assert_eq!(sc.get("exit_code").and_then(Value::as_i64), Some(0));

    // win32_pty_exec -> ptyExec, with cols/rows mapped.
    client
        .call_tool(
            CallToolRequestParams::new("win32_pty_exec").with_arguments(
                json!({"argv": ["cmd"], "cols": 120, "rows": 40})
                    .as_object()
                    .unwrap()
                    .clone(),
            ),
        )
        .await
        .expect("pty_exec ok");
    let (cmd, args) = dev.last();
    assert_eq!(cmd, "ptyExec");
    assert_eq!(args.get("cols").and_then(Value::as_i64), Some(120));
    assert_eq!(args.get("rows").and_then(Value::as_i64), Some(40));

    // win32_list_commands -> listCommands (no args).
    let res = client
        .call_tool(CallToolRequestParams::new("win32_list_commands"))
        .await
        .expect("list_commands ok");
    let (cmd, _args) = dev.last();
    assert_eq!(cmd, "listCommands");
    let sc = res.structured_content.expect("list_commands is structured");
    assert!(sc.get("commands").and_then(Value::as_array).is_some());

    // Exactly one of each verb reached the device.
    assert_eq!(dev.count("exec"), 1);
    assert_eq!(dev.count("ptyExec"), 1);
    assert_eq!(dev.count("listCommands"), 1);
    let _ = client.cancel().await;
}

/// A device status:error on an exec verb is a recoverable isError (not a panic)
/// — the exec-seam representative of the generic ToolCallRecoverableError
/// mapping (the full mapping lives in integration.rs).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn exec_device_error_is_iserror() {
    let client = serve(
        caps(false, false),
        Box::new(mock::RecordingDevice::error("catalog miss")),
    )
    .await;
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_exec")
                .with_arguments(json!({"argv": ["nope"]}).as_object().unwrap().clone()),
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
