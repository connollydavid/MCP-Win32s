//! mcp-w32s-bridge entry point.
//!
//! Connects to the Win32 device, completes the ready handshake, then
//! serves MCP over stdio. Logging goes to stderr — stdout is the MCP
//! protocol channel and must carry only JSON-RPC.

use anyhow::Result;
use rmcp::transport::stdio;
use rmcp::ServiceExt;

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_max_level(tracing::Level::INFO)
        .init();

    let (caps, device, audit_log) =
        mcp_w32s_bridge::device::connect(std::env::args().skip(1)).await?;
    tracing::info!(version = %caps.version, codepage = caps.codepage, "device ready");

    // The power-tool audit sink: a file (from `--audit-log`) or stderr.
    let audit = match audit_log {
        Some(path) => mcp_w32s_bridge::audit::AuditLog::file_default(path),
        None => mcp_w32s_bridge::audit::AuditLog::stderr(),
    };

    let bridge = mcp_w32s_bridge::server::Bridge::with_config(
        caps,
        device,
        audit,
        mcp_w32s_bridge::breaker::Breaker::production(),
        mcp_w32s_bridge::ratelimit::RateLimiter::production(),
    );
    let service = bridge.serve(stdio()).await?;
    service.waiting().await?;
    Ok(())
}
