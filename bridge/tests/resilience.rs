//! 5.5 integration tests: runtime resilience and the power-tool audit. A
//! tripped circuit-breaker and an exhausted rate limiter refuse a call LOCALLY
//! (recoverable isError, device untouched — ToolCallCircuitOpen /
//! ToolCallRateLimited); every power-tool call produces a durable audit record
//! across every outcome path, while a read-only tool produces none
//! (PowerToolsAreAudited). Cites bridge/OBLIGATIONS-5.5.md.
//!
//! The breaker/limiter are pre-tripped via the injectable Bridge::with_config
//! constructor (a tiny threshold/cap and a long cooldown so the trip holds for
//! the test) — the tests pin the REFUSAL behaviour, not the trip policy.

mod common;

use async_trait::async_trait;
use mcp_w32s_bridge::audit::AuditLog;
use mcp_w32s_bridge::breaker::{Breaker, SystemClock};
use mcp_w32s_bridge::capabilities::{Capabilities, EncodingProvenance, MemTier};
use mcp_w32s_bridge::device::Device;
use mcp_w32s_bridge::ratelimit::RateLimiter;
use mcp_w32s_bridge::server::Bridge;
use mcp_w32s_bridge::wire::{Command, Response};
use rmcp::model::CallToolRequestParams;
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{json, Value};
use std::sync::{Arc, Mutex};
use std::time::Duration;

fn caps() -> Capabilities {
    Capabilities {
        has_pty: true,
        mem: MemTier::None,
        encoding: EncodingProvenance::FromCodepage,
        codepage: 437,
        version: "test".to_string(),
        toolchains: vec![],
        toolchain_registration: false,
        allow_memory_write: false,
        allow_unsafe_exec: true,
    }
}

/// A device that records each command and acks ok — so a test can assert it was
/// NOT touched when the bridge refused locally.
#[derive(Clone)]
struct CountingDevice {
    received: Arc<Mutex<Vec<String>>>,
}

impl CountingDevice {
    fn new() -> Self {
        CountingDevice {
            received: Arc::new(Mutex::new(Vec::new())),
        }
    }
    fn is_empty(&self) -> bool {
        self.received.lock().unwrap().is_empty()
    }
}

#[async_trait]
impl Device for CountingDevice {
    async fn call(&mut self, cmd: &Command) -> Result<Response, anyhow::Error> {
        self.received.lock().unwrap().push(cmd.cmd.clone());
        let reply = match cmd.cmd.as_str() {
            "listCommands" => json!({"id": cmd.id, "status": "ok", "commands": []}),
            _ => json!({
                "id": cmd.id, "status": "ok", "exit_code": 0,
                "stdout_b64": "", "stderr_b64": ""
            }),
        };
        Ok(serde_json::from_value(reply)?)
    }
}

/// A breaker pre-tripped Open: threshold 1, a long cooldown so it stays open for
/// the test, the real clock.
fn tripped_breaker() -> Breaker {
    let mut b = Breaker::new(1, Duration::from_secs(3600), Box::new(SystemClock));
    b.record_failure();
    b
}

/// A limiter with a generous cap and the real clock (no shedding).
fn open_limiter() -> RateLimiter {
    RateLimiter::new(1000, Duration::from_secs(1), Box::new(SystemClock))
}

/// A limiter pre-exhausted for `tool`: cap 1, the one token spent, a long
/// refill interval so it stays empty for the test.
fn exhausted_limiter(tool: &str) -> RateLimiter {
    let mut rl = RateLimiter::new(1, Duration::from_secs(3600), Box::new(SystemClock));
    assert!(rl.try_acquire(tool), "spend the single token");
    rl
}

/// A breaker that never opens (threshold high), the real clock.
fn closed_breaker() -> Breaker {
    Breaker::new(1000, Duration::from_secs(1), Box::new(SystemClock))
}

async fn serve(
    caps: Capabilities,
    device: Box<dyn Device>,
    audit: AuditLog,
    breaker: Breaker,
    limiter: RateLimiter,
) -> RunningService<RoleClient, ()> {
    let (server_t, client_t) = tokio::io::duplex(16384);
    let bridge = Bridge::with_config(caps, device, audit, breaker, limiter);
    tokio::spawn(async move {
        if let Ok(server) = bridge.serve(server_t).await {
            let _ = server.waiting().await;
        }
    });
    ().serve(client_t).await.expect("client up")
}

fn error_text(res: &rmcp::model::CallToolResult) -> String {
    res.content
        .iter()
        .find_map(|c| c.as_text().map(|t| t.text.clone()))
        .expect("an error text block")
}

/// rule-success.ToolCallCircuitOpen (+creation.1) — a tripped breaker makes
/// win32_exec a recoverable isError "device circuit open"; the device is never
/// touched.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn circuit_open_refuses_locally() {
    let dev = CountingDevice::new();
    let client = serve(
        caps(),
        Box::new(dev.clone()),
        AuditLog::stderr(),
        tripped_breaker(),
        open_limiter(),
    )
    .await;
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_exec")
                .with_arguments(json!({"argv": ["dir"]}).as_object().unwrap().clone()),
        )
        .await
        .expect("the call returns with a tool error");
    assert_eq!(
        res.is_error,
        Some(true),
        "circuit open is a recoverable isError"
    );
    assert_eq!(error_text(&res), "device circuit open");
    assert!(
        res.structured_content.is_none(),
        "a local refusal has no structuredContent"
    );
    assert!(dev.is_empty(), "the device was NOT touched (circuit open)");
    let _ = client.cancel().await;
}

/// rule-success.ToolCallRateLimited (+creation.1) — an exhausted per-tool rate
/// cap makes win32_exec a recoverable isError "rate limited"; the device is
/// never touched.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn rate_limit_sheds_locally() {
    let dev = CountingDevice::new();
    let client = serve(
        caps(),
        Box::new(dev.clone()),
        AuditLog::stderr(),
        closed_breaker(),
        exhausted_limiter("win32_exec"),
    )
    .await;
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_exec")
                .with_arguments(json!({"argv": ["dir"]}).as_object().unwrap().clone()),
        )
        .await
        .expect("the call returns with a tool error");
    assert_eq!(
        res.is_error,
        Some(true),
        "rate limited is a recoverable isError"
    );
    assert_eq!(error_text(&res), "rate limited");
    assert!(
        res.structured_content.is_none(),
        "a local refusal has no structuredContent"
    );
    assert!(dev.is_empty(), "the device was NOT touched (rate limited)");
    let _ = client.cancel().await;
}

/// @guarantee PowerToolsAreAudited — a power-tool call produces a durable audit
/// record across the outcome paths (relayed, refused locally for circuit open /
/// rate limited / unsafe opt-in absent, device error); a read-only tool
/// (win32_list_commands) produces NONE. Asserted by reading the audit file.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn power_tool_calls_are_audited() {
    let dir = std::env::temp_dir().join(format!("bridge-resilience-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);

    // --- relayed: a plain exec on a healthy bridge audits outcome:relayed, and
    //     a read-only list_commands produces NO record.
    {
        let path = dir.join("relayed.log");
        let _ = std::fs::remove_file(&path);
        let client = serve(
            caps(),
            Box::new(CountingDevice::new()),
            AuditLog::file(&path, 8 * 1024 * 1024),
            closed_breaker(),
            open_limiter(),
        )
        .await;
        client
            .call_tool(
                CallToolRequestParams::new("win32_exec")
                    .with_arguments(json!({"argv": ["dir"]}).as_object().unwrap().clone()),
            )
            .await
            .expect("exec ok");
        client
            .call_tool(CallToolRequestParams::new("win32_list_commands"))
            .await
            .expect("list_commands ok");
        let _ = client.cancel().await;

        let records = read_records(&path);
        let exec_recs: Vec<&Value> = records
            .iter()
            .filter(|r| r.get("tool").and_then(Value::as_str) == Some("win32_exec"))
            .collect();
        assert_eq!(
            exec_recs.len(),
            1,
            "the exec call produced one audit record"
        );
        assert_eq!(
            exec_recs[0].get("outcome").and_then(Value::as_str),
            Some("relayed")
        );
        assert!(
            !records
                .iter()
                .any(|r| r.get("tool").and_then(Value::as_str) == Some("win32_list_commands")),
            "the read-only list_commands produces NO audit record"
        );
    }

    // --- circuit open: the refusal is audited outcome:device_circuit_open.
    assert_refusal_audited(
        &dir.join("breaker.log"),
        tripped_breaker(),
        open_limiter(),
        json!({"argv": ["dir"]}),
        "device_circuit_open",
    )
    .await;

    // --- rate limited: the refusal is audited outcome:rate_limited.
    assert_refusal_audited(
        &dir.join("rate.log"),
        closed_breaker(),
        exhausted_limiter("win32_exec"),
        json!({"argv": ["dir"]}),
        "rate_limited",
    )
    .await;

    // --- unsafe opt-in absent: with allow_unsafe_exec:false the unsafe exec is
    //     refused locally and audited outcome:unsafe_opt_in_absent.
    {
        let path = dir.join("unsafe.log");
        let _ = std::fs::remove_file(&path);
        let mut c = caps();
        c.allow_unsafe_exec = false;
        let client = serve(
            c,
            Box::new(CountingDevice::new()),
            AuditLog::file(&path, 8 * 1024 * 1024),
            closed_breaker(),
            open_limiter(),
        )
        .await;
        client
            .call_tool(
                CallToolRequestParams::new("win32_exec").with_arguments(
                    json!({"argv": ["calc"], "unsafe": true})
                        .as_object()
                        .unwrap()
                        .clone(),
                ),
            )
            .await
            .expect("the refusal returns a tool error");
        let _ = client.cancel().await;
        let records = read_records(&path);
        assert_eq!(records.len(), 1, "the unsafe refusal produced one record");
        assert_eq!(
            records[0].get("outcome").and_then(Value::as_str),
            Some("unsafe_opt_in_absent")
        );
        assert_eq!(
            records[0].get("tool").and_then(Value::as_str),
            Some("win32_exec")
        );
    }

    // --- device error: a status:error reply audits outcome:device_error.
    {
        let path = dir.join("deverr.log");
        let _ = std::fs::remove_file(&path);
        let client = serve(
            caps(),
            Box::new(common::RecordingDevice::error("boom")),
            AuditLog::file(&path, 8 * 1024 * 1024),
            closed_breaker(),
            open_limiter(),
        )
        .await;
        client
            .call_tool(
                CallToolRequestParams::new("win32_exec")
                    .with_arguments(json!({"argv": ["dir"]}).as_object().unwrap().clone()),
            )
            .await
            .expect("the device error returns a tool error");
        let _ = client.cancel().await;
        let records = read_records(&path);
        assert_eq!(records.len(), 1, "the device error produced one record");
        assert_eq!(
            records[0].get("outcome").and_then(Value::as_str),
            Some("device_error")
        );
    }

    let _ = std::fs::remove_dir_all(&dir);
}

/// Drive one exec call that the bridge refuses locally, asserting exactly one
/// audit record with the expected outcome.
async fn assert_refusal_audited(
    path: &std::path::Path,
    breaker: Breaker,
    limiter: RateLimiter,
    argv_args: Value,
    expected_outcome: &str,
) {
    let _ = std::fs::remove_file(path);
    let dev = CountingDevice::new();
    let client = serve(
        caps(),
        Box::new(dev.clone()),
        AuditLog::file(path, 8 * 1024 * 1024),
        breaker,
        limiter,
    )
    .await;
    client
        .call_tool(
            CallToolRequestParams::new("win32_exec")
                .with_arguments(argv_args.as_object().unwrap().clone()),
        )
        .await
        .expect("the refusal returns a tool error");
    let _ = client.cancel().await;
    assert!(dev.is_empty(), "a local refusal does not touch the device");
    let records = read_records(path);
    assert_eq!(
        records.len(),
        1,
        "exactly one audit record for {expected_outcome}"
    );
    assert_eq!(
        records[0].get("outcome").and_then(Value::as_str),
        Some(expected_outcome)
    );
    assert_eq!(
        records[0].get("tool").and_then(Value::as_str),
        Some("win32_exec")
    );
}

/// Parse each JSON line of an audit file.
fn read_records(path: &std::path::Path) -> Vec<Value> {
    let body = std::fs::read_to_string(path).unwrap_or_default();
    body.lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str(l).expect("a valid audit JSON line"))
        .collect()
}
