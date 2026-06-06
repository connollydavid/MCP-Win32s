//! 5.0 integration tests: the bridge's tool-call -> device round-trip ->
//! result mapping, against the mock device. Each cites its obligation
//! IDs from bridge/OBLIGATIONS-5.0.md.

mod common;
use common as mock;

use mcp_w32s_bridge::capabilities::{tools_to_prune, Capabilities, EncodingMode, MemTier};
use mcp_w32s_bridge::server::{Bridge, EchoParams};
use rmcp::handler::server::wrapper::Parameters;

fn caps(pty: bool) -> Capabilities {
    Capabilities {
        has_pty: pty,
        mem: MemTier::None,
        encoding: EncodingMode::Codepage,
        codepage: 437,
        version: "test".to_string(),
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
/// gate mechanism. 5.0 ships no gated tools, so the prune set is empty for
/// any capability profile; the per-capability logic is property-tested in
/// props.rs.
#[test]
fn gating_prune_set_is_empty_in_5_0() {
    assert!(tools_to_prune(&caps(false)).is_empty());
    assert!(tools_to_prune(&caps(true)).is_empty());
}
