//! Integration tests: the bridge's tool-call -> device round-trip ->
//! result mapping, against the mock device. Each cites its obligation
//! IDs from bridge/OBLIGATIONS-5.0.md.

mod common;
use common as mock;

use mcp_w32s_bridge::capabilities::{tools_to_prune, Capabilities, EncodingProvenance, MemTier};
use mcp_w32s_bridge::server::{Bridge, EchoParams};
use rmcp::handler::server::wrapper::Parameters;

fn caps(pty: bool) -> Capabilities {
    Capabilities {
        has_pty: pty,
        mem: MemTier::None,
        encoding: EncodingProvenance::FromCodepage,
        codepage: 437,
        version: "test".to_string(),
        toolchains: vec![],
        toolchain_registration: false,
        allow_memory_write: false,
        allow_unsafe_exec: false,
    }
}

/// rule-success.ToolCallSucceeded, transition-edge.ToolCall.dispatched.ok,
/// invariant.SuccessResultsAreStructured — device ok maps to a success
/// result with structuredContent (and a text mirror).
#[tokio::test]
async fn echo_ok_maps_to_structured_success() {
    let bridge = Bridge::new(caps(false), Box::new(mock::MockDevice));
    let r = bridge
        .win32_echo(Parameters(EchoParams {
            text: "hello".to_string(),
        }))
        .await
        .unwrap();
    assert_eq!(r.is_error, Some(false), "ok is not an error");
    let sc = r.structured_content.expect("ok carries structuredContent");
    assert_eq!(sc.get("data").and_then(|v| v.as_str()), Some("hello"));
    assert!(!r.content.is_empty(), "ok carries a text mirror too");
}

/// rule-success.ToolCallRecoverableError, when-presence.ToolCall.failure_kind,
/// invariant.DispatchedErrorsAreRecoverable — a device status:"error" is a
/// recoverable isError tool result, not a protocol error.
#[tokio::test]
async fn device_error_maps_to_iserror() {
    let bridge = Bridge::new(caps(false), Box::new(mock::MockDevice));
    let r = bridge
        .win32_echo(Parameters(EchoParams {
            text: "BOOM".to_string(),
        }))
        .await
        .unwrap();
    assert_eq!(r.is_error, Some(true), "device error -> isError");
    // The spec's ToolCallRecoverableError ensures `text: reason` — the
    // device's error string must surface to the model, not be dropped.
    let text: String = r
        .content
        .iter()
        .filter_map(|c| c.as_text())
        .map(|t| t.text.as_str())
        .collect();
    assert!(
        text.contains("unknown command"),
        "device error reason surfaces in the result text; got {text:?}"
    );
}

/// A transport failure is also a recoverable isError result (the model
/// can retry), never a panic.
#[tokio::test]
async fn transport_failure_maps_to_iserror() {
    let bridge = Bridge::new(caps(false), Box::new(mock::DeadDevice));
    let r = bridge
        .win32_echo(Parameters(EchoParams {
            text: "x".to_string(),
        }))
        .await
        .unwrap();
    assert_eq!(r.is_error, Some(true), "dead link -> isError");
}

/// rule-success.ToolAdvertised, invariant.AdvertisedToolsAreCapable — the
/// gate mechanism. The FIRST real gated tools (the five memory
/// tools) close the G1 gap that had left this prune set empty. The
/// `caps()` helper here is a `mem: none` device, so every memory tool is
/// pruned regardless of pty. (The per-capability logic is property-tested in
/// props.rs; the tools/list effect in memory.rs.)
#[test]
fn memory_tools_pruned_when_uncapable() {
    for pty in [false, true] {
        let pruned = tools_to_prune(&caps(pty));
        for name in [
            "win32_spawn_retain",
            "win32_peek",
            "win32_poke",
            "win32_terminate",
            "win32_release",
        ] {
            assert!(
                pruned.contains(&name),
                "{name} is pruned on a mem:none device"
            );
        }
    }
}

/// Full MCP lifecycle over an in-memory duplex (no spawn, no network):
/// real initialize/negotiation, tools/list reflecting the surface,
/// tools/call dispatch+mapping, and an unknown tool as a JSON-RPC
/// PROTOCOL error (never reaching the device).
/// Obligations: surface-*.McpClient, rule-success.{ToolCallDispatched,
/// ToolCallSucceeded}, rule-success.ToolCallRejected (protocol error),
/// invariant.ProtocolFailuresNeverDispatch.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn full_mcp_lifecycle_over_duplex() {
    use rmcp::model::CallToolRequestParams;
    use rmcp::ServiceExt;
    use serde_json::json;

    let (server_t, client_t) = tokio::io::duplex(8192);
    let bridge = Bridge::new(caps(false), Box::new(mock::MockDevice));
    // Drive the server on its own task (matches the rmcp test pattern); the
    // client runs in the test body.
    tokio::spawn(async move {
        if let Ok(server) = bridge.serve(server_t).await {
            let _ = server.waiting().await;
        }
    });
    let client = ().serve(client_t).await.expect("client up");

    let tools = client
        .list_tools(Default::default())
        .await
        .expect("list_tools");
    assert!(
        tools.tools.iter().any(|t| t.name == "win32_echo"),
        "win32_echo is advertised"
    );

    let args = json!({"text": "ping"}).as_object().unwrap().clone();
    let res = client
        .call_tool(CallToolRequestParams::new("win32_echo").with_arguments(args))
        .await
        .expect("call_tool ok");
    assert_eq!(res.is_error, Some(false));
    assert_eq!(
        res.structured_content
            .as_ref()
            .and_then(|v| v.get("data"))
            .and_then(|v| v.as_str()),
        Some("ping")
    );

    let err = client
        .call_tool(CallToolRequestParams::new("does_not_exist"))
        .await;
    assert!(err.is_err(), "unknown tool -> JSON-RPC protocol error");

    let _ = client.cancel().await;
}

/// rule.VersionNegotiated, invariant.{ReadySessionHasVersion,
/// NegotiatedVersionIsCommonFloor} — the bridge delegates negotiation to
/// rmcp, which down-negotiates to the LOWER (earlier) of {client, bridge}.
/// A client requesting an older supported revision gets its OWN version
/// echoed back (not clamped up to the bridge's latest); a client at the
/// bridge's own version negotiates to that. This pins the corrected
/// VersionNegotiated branch against the real library so a future rmcp bump
/// that changes negotiation is caught.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn version_negotiation_is_down_to_the_common_floor() {
    use rmcp::model::{ClientInfo, ProtocolVersion};
    use rmcp::{ClientHandler, ServiceExt};

    // A client whose advertised protocol version we control.
    #[derive(Clone)]
    struct VersionedClient(ProtocolVersion);
    impl ClientHandler for VersionedClient {
        fn get_info(&self) -> ClientInfo {
            let mut info = ClientInfo::default();
            info.protocol_version = self.0.clone();
            info
        }
    }

    // The negotiated version is the server's returned InitializeResult
    // version (peer_info), which the bridge/rmcp set to the common floor.
    async fn negotiate(client_version: Option<ProtocolVersion>) -> ProtocolVersion {
        let (server_t, client_t) = tokio::io::duplex(8192);
        let bridge = Bridge::new(caps(false), Box::new(mock::MockDevice));
        tokio::spawn(async move {
            if let Ok(s) = bridge.serve(server_t).await {
                let _ = s.waiting().await;
            }
        });
        let negotiated = match client_version {
            Some(v) => {
                let c = VersionedClient(v).serve(client_t).await.expect("client up");
                let n = c.peer_info().expect("server info").protocol_version.clone();
                let _ = c.cancel().await;
                n
            }
            None => {
                let c = ().serve(client_t).await.expect("client up");
                let n = c.peer_info().expect("server info").protocol_version.clone();
                let _ = c.cancel().await;
                n
            }
        };
        negotiated
    }

    // Older supported client -> echoed down to the client's own version,
    // NOT the bridge's 2025-11-25 (the bug the spec's old else-branch had).
    assert_eq!(
        negotiate(Some(ProtocolVersion::V_2025_06_18)).await,
        ProtocolVersion::V_2025_06_18,
        "older client -> down-negotiated to the client's own version"
    );

    // Client at the bridge's latest (the default ClientInfo) -> negotiated
    // stays at the bridge's version; non-empty (ReadySessionHasVersion).
    assert_eq!(
        negotiate(None).await,
        ProtocolVersion::V_2025_11_25,
        "equal client -> negotiated is the bridge's latest"
    );
}
