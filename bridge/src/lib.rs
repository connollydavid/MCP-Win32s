//! MCP bridge for the MCP-Win32s device (Phase 5.0, bridge core).
//!
//! An MCP server (stdio, via `rmcp`) that fronts a Win32s..Win11 device
//! reachable over the frozen newline-JSON wire protocol (serial/TCP).
//! Implements `specs/mcp-bridge.allium`. rmcp provides the MCP lifecycle
//! and lenient version negotiation; this crate provides the device link,
//! the capability gating, and the tool-call -> device -> result mapping
//! with the isError-vs-protocol-error split.

pub mod capabilities;
pub mod device;
pub mod server;
pub mod wire;
