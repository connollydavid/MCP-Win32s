//! The build-toolchain subsystem: toolchains as author-supplied DATA. A
//! `ToolchainDefinition` (built-in MSVC/Open Watcom, operator-config, or
//! runtime-registered) declares how to detect, drive, and parse a build
//! toolchain; the bridge generates `win32_<vendor>_<role>` tools from it,
//! routing each through the device's catalogued `exec` and parsing the output
//! into structured diagnostics.
//!
//! Implements `specs/toolchains.allium` and the build constructs of
//! `specs/mcp-bridge.allium`; the authoring format is documented in
//! `docs/toolchain-definition-guide.md`. The rmcp tool-generation seam lives in
//! `server.rs` (it wires these modules into dynamic `ToolRoute`s).

pub mod argv;
pub mod definition;
pub mod diagnostics;
