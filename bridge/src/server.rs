//! The MCP server (`rmcp`): the tool surface, the tool-call -> device
//! round-trip -> result mapping, and the isError-vs-protocol split.
//! rmcp provides the MCP lifecycle and lenient version negotiation;
//! unknown-tool / malformed-argument calls are rmcp's JSON-RPC protocol
//! errors (they never reach the device). A recoverable device failure is
//! mapped to an isError tool result the model can fix or retry.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, Content, ServerCapabilities, ServerInfo};
use rmcp::{tool, tool_handler, tool_router, ErrorData, ServerHandler};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tokio::sync::Mutex;

use crate::capabilities::{tools_to_prune, Capabilities};
use crate::device::Device;
use crate::wire::Command;

/// Parameters for `win32_echo`.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct EchoParams {
    /// Text to echo back through the device (round-trip check).
    pub text: String,
}

struct Inner {
    caps: Capabilities,
    device: Mutex<Box<dyn Device>>,
    counter: AtomicU64,
}

/// The bridge MCP server. Clone-cheap (shares one `Inner`); rmcp requires
/// the handler to be `Clone`.
#[derive(Clone)]
pub struct Bridge {
    inner: Arc<Inner>,
    tool_router: ToolRouter<Bridge>,
}

impl Bridge {
    /// Build the server from the resolved device capabilities and the
    /// live device client. Capability gating: the full router is built,
    /// then tools whose required capability the device lacks are pruned,
    /// so `tools/list` reflects the device.
    pub fn new(caps: Capabilities, device: Box<dyn Device>) -> Self {
        let mut router = Self::tool_router();
        for name in tools_to_prune(&caps) {
            router.remove_route(name);
        }
        Bridge {
            inner: Arc::new(Inner {
                caps,
                device: Mutex::new(device),
                counter: AtomicU64::new(1),
            }),
            tool_router: router,
        }
    }

    fn next_id(&self) -> String {
        let n = self.inner.counter.fetch_add(1, Ordering::Relaxed);
        format!("b{n}")
    }

    /// Round-trip a command to the device and map the reply: ok -> a
    /// success result carrying `fields` as structuredContent; a device
    /// `status:"error"` -> an isError tool result; a transport failure ->
    /// an isError tool result (recoverable from the model's view).
    async fn round_trip(&self, cmd: Command) -> CallToolResult {
        let result = {
            let mut dev = self.inner.device.lock().await;
            dev.call(&cmd).await
        };
        match result {
            // ok -> structuredContent (rmcp's `structured` also mirrors the
            // JSON into a text content block for clients that only read text).
            Ok(resp) if resp.is_ok() => CallToolResult::structured(json!(resp.fields)),
            // A device status:"error" is a recoverable tool error (isError).
            Ok(resp) => {
                let reason = resp.error.unwrap_or_else(|| "device error".to_string());
                CallToolResult::error(vec![Content::text(reason)])
            }
            // A transport failure is also recoverable from the model's view.
            Err(e) => {
                CallToolResult::error(vec![Content::text(format!("device unreachable: {e}"))])
            }
        }
    }
}

#[tool_router(router = tool_router)]
impl Bridge {
    /// Echo text back through the Win32 device — proves the round-trip,
    /// the ok mapping (text + structuredContent), and (on a device error)
    /// the isError path. The capability surface proper arrives in 5.1-5.4.
    #[tool(
        name = "win32_echo",
        description = "Echo text back through the Win32 device (a round-trip connectivity check)."
    )]
    pub async fn win32_echo(
        &self,
        Parameters(p): Parameters<EchoParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("echo", self.next_id()).with("line", Value::String(p.text));
        Ok(self.round_trip(cmd).await)
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for Bridge {
    fn get_info(&self) -> ServerInfo {
        // protocol_version defaults to LATEST (2025-11-25); rmcp negotiates
        // leniently down to the client. server_info defaults from the crate.
        ServerInfo::new(ServerCapabilities::builder().enable_tools().build()).with_instructions(
            format!(
                "Bridge to a Win32 device ({}, codepage {}). Tools are gated by device capability.",
                self.inner.caps.version, self.inner.caps.codepage
            ),
        )
    }
}
