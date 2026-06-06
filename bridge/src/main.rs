//! mcp-w32s-bridge entry point (Phase 5.0).
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

    let (caps, device) = mcp_w32s_bridge::device::connect(std::env::args().skip(1)).await?;
    tracing::info!(version = %caps.version, codepage = caps.codepage, "device ready");

    let bridge = mcp_w32s_bridge::server::Bridge::new(caps, device);
    let service = bridge.serve(stdio()).await?;
    service.waiting().await?;
    Ok(())
}
