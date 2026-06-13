//! The MCP server (`rmcp`): the tool surface, the tool-call -> device
//! round-trip -> result mapping, and the isError-vs-protocol split.
//! rmcp provides the MCP lifecycle and lenient version negotiation;
//! unknown-tool / malformed-argument calls are rmcp's JSON-RPC protocol
//! errors (they never reach the device). A recoverable device failure is
//! mapped to an isError tool result the model can fix or retry.

use std::collections::HashSet;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use base64::Engine;
use rmcp::handler::server::router::tool::{ToolRoute, ToolRouter};
use rmcp::handler::server::tool::ToolCallContext;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{
    object, CallToolResult, Content, ServerCapabilities, ServerInfo, Tool, ToolAnnotations,
};
use rmcp::{tool, tool_handler, tool_router, ErrorData, ServerHandler};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use tokio::sync::Mutex;

use crate::audit::{AuditLog, Outcome};
use crate::breaker::Breaker;
use crate::capabilities::{tools_to_prune, Capabilities};
use crate::device::Device;
use crate::ratelimit::RateLimiter;
use crate::toolchain::argv;
use crate::toolchain::definition::{
    self, ArgItem, DefinitionSource, DiagnosticSpec, Registry, RoleSpec, ToolRole,
};
use crate::toolchain::diagnostics;
use crate::wire::Command;

/// Wall-clock ceiling for a build exec (ms). A compile/link that overruns is
/// killed device-side and surfaces as a timed-out exec error.
const BUILD_TIMEOUT_MS: i64 = 120_000;

/// The committed authoring guide, embedded so `win32_list_toolchains` can
/// return it inline (the discovery tool returns schema + definitions + guide).
const TOOLCHAIN_GUIDE: &str = include_str!("../../docs/toolchain-definition-guide.md");

/// Parameters for `win32_echo`.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct EchoParams {
    /// Text to echo back through the device (round-trip check).
    pub text: String,
}

/// Parameters for the single-path file tools (read/list/delete/mkdir/rmdir).
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct PathParams {
    /// Absolute or device-relative path on the Win32 host.
    pub path: String,
}

/// Parameters for `win32_write_file`: a path and the base64-encoded bytes.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct WriteParams {
    /// Path to write on the Win32 host.
    pub path: String,
    /// File contents, base64-encoded (the wire is base64 for binary safety).
    pub data: String,
}

/// Parameters for the two-path file tools (copy/move): source and dest.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct SourceDestParams {
    /// Existing source path on the Win32 host.
    pub source: String,
    /// Destination path on the Win32 host (must not already exist).
    pub dest: String,
}

/// Parameters for `win32_spawn_retain`: launch and retain a process as a
/// memory target. `argv[0]` must be a catalogued command (the device
/// re-gates it — SpawnRetainCommandIsCatalogued).
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct SpawnRetainParams {
    /// The command to launch, as an argv array. `argv[0]` is the catalogued
    /// command name; the rest are its arguments.
    pub argv: Vec<String>,
    /// Working directory for the child, or omit to inherit the device's.
    #[serde(default)]
    pub cwd: Option<String>,
}

/// Parameters for `win32_peek`: read memory.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct PeekParams {
    /// The spawn-retain token (from `win32_spawn_retain`) naming the process
    /// to read. Required on the NT+ `process` tier; omit on the pre-NT
    /// `arena`/`shared_vm` tiers, which read the shared address space directly.
    #[serde(default)]
    pub token: Option<String>,
    /// The address to read, as a string: hex (`0x...`) or decimal, in
    /// `[0, 0xFFFFFFFF]` (a 32-bit address does not fit a JSON integer).
    pub addr: String,
    /// The number of bytes to read (at most 65536).
    pub len: u32,
}

/// Parameters for `win32_poke`: write memory (destructive; gated).
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct PokeParams {
    /// The spawn-retain token naming the process to write. Required on the
    /// NT+ `process` tier; omit on the pre-NT tiers.
    #[serde(default)]
    pub token: Option<String>,
    /// The address to write, as a string: hex (`0x...`) or decimal.
    pub addr: String,
    /// The bytes to write, base64-encoded. The write length is the decoded
    /// byte count (at most 65536).
    pub data: String,
}

/// Parameters for the token-only memory tools (`win32_terminate`,
/// `win32_release`).
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct TokenParams {
    /// The spawn-retain token to act on (from `win32_spawn_retain`).
    pub token: String,
}

/// Parameters for `win32_exec`: run a catalogued command capturing output.
/// `argv[0]` must be on the device catalog (the device re-gates it) unless
/// `unsafe` is set and the operator armed `--allow-unsafe-exec`. Every optional
/// field maps by name onto the device `exec` wire command.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct ExecParams {
    /// The command to run, as an argv array. `argv[0]` is the (catalogued)
    /// command; the rest are its arguments.
    pub argv: Vec<String>,
    /// Working directory for the child, or omit to inherit the device's.
    #[serde(default)]
    pub cwd: Option<String>,
    /// Wall-clock timeout in milliseconds; the child is killed if it overruns.
    #[serde(default)]
    pub timeout_ms: Option<i64>,
    /// Standard input for the child, base64-encoded.
    #[serde(default)]
    pub stdin_b64: Option<String>,
    /// Maximum captured output bytes per stream (the device truncates beyond).
    #[serde(default)]
    pub max_output: Option<i64>,
    /// Run the command line through the shell (`cmd /c`) rather than spawning
    /// the program directly. The device neutralises the user tail either way.
    #[serde(default)]
    pub shell: Option<bool>,
    /// Address-space cap for the child in bytes (a job-object memory limit on
    /// capable hosts).
    #[serde(default)]
    pub mem_cap_bytes: Option<i64>,
    /// CPU-time cap for the child in milliseconds (a job-object CPU limit on
    /// capable hosts).
    #[serde(default)]
    pub cpu_time_ms: Option<i64>,
    /// Bypass the device command catalog allow-list (THE UNSAFE BYPASS). Off by
    /// default; relayed to the device only when the operator armed the bridge
    /// with `--allow-unsafe-exec`, otherwise the call is refused locally
    /// (UnsafeExecRequiresOperatorOptIn). The wire field is literally `unsafe`;
    /// the Rust field is renamed because `unsafe` is a keyword.
    #[serde(rename = "unsafe", default)]
    pub unsafe_bypass: Option<bool>,
}

/// Parameters for `win32_pty_exec`: like exec, run under a pseudo-console, with
/// terminal dimensions instead of the resource caps. Maps by name onto the
/// device `ptyExec` wire command.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct PtyExecParams {
    /// The command to run, as an argv array.
    pub argv: Vec<String>,
    /// Working directory for the child, or omit to inherit the device's.
    #[serde(default)]
    pub cwd: Option<String>,
    /// Wall-clock timeout in milliseconds.
    #[serde(default)]
    pub timeout_ms: Option<i64>,
    /// Standard input for the child, base64-encoded.
    #[serde(default)]
    pub stdin_b64: Option<String>,
    /// Maximum captured output bytes (the device truncates beyond).
    #[serde(default)]
    pub max_output: Option<i64>,
    /// Run the command line through the shell rather than spawning directly.
    #[serde(default)]
    pub shell: Option<bool>,
    /// Pseudo-console width in columns.
    #[serde(default)]
    pub cols: Option<i64>,
    /// Pseudo-console height in rows.
    #[serde(default)]
    pub rows: Option<i64>,
}

/// Parameters for `win32_list_commands`: none — it reports the device catalog.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[schemars(deny_unknown_fields)]
pub struct ListCommandsParams {}

struct Inner {
    caps: Capabilities,
    device: Mutex<Box<dyn Device>>,
    counter: AtomicU64,
    /// The loaded toolchain definitions (built-ins + any runtime-registered).
    /// Build-tool routes are generated from this at construction; runtime
    /// registration mutates it (the new tools materialise on the next session,
    /// per the registry-as-source-of-truth model).
    registry: Mutex<Registry>,
    /// Runtime resilience: the device-facing circuit-breaker and the
    /// per-tool token-bucket rate limiter every relayed call passes through
    /// (rate-limit -> breaker -> relay), plus the append-only power-tool audit
    /// log (PowerToolsAreAudited).
    breaker: Mutex<Breaker>,
    limiter: Mutex<RateLimiter>,
    audit: AuditLog,
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
    /// live device client, with the default resilience config (a production
    /// circuit-breaker + rate limiter, audit to stderr). Capability gating: the
    /// full router is built, then tools whose required capability the device
    /// lacks are pruned, so `tools/list` reflects the device.
    pub fn new(caps: Capabilities, device: Box<dyn Device>) -> Self {
        Self::with_config(
            caps,
            device,
            AuditLog::stderr(),
            Breaker::production(),
            RateLimiter::production(),
        )
    }

    /// Build the server with explicit resilience config: the power-tool audit
    /// sink, the device circuit-breaker, and the per-tool rate limiter. `main`
    /// uses this to wire the `--audit-log` sink; tests use it to inject a
    /// tripped breaker / a tiny rate cap so the local-refusal paths fire
    /// deterministically.
    pub fn with_config(
        caps: Capabilities,
        device: Box<dyn Device>,
        audit: AuditLog,
        breaker: Breaker,
        limiter: RateLimiter,
    ) -> Self {
        let mut router = Self::tool_router();
        for name in tools_to_prune(&caps) {
            router.remove_route(name);
        }

        // The build subsystem: definitions are author-supplied DATA. Seed the
        // registry with the two built-in reference definitions, generate one
        // win32_<name>_<role> tool per supported (definition, role) the device
        // detected, then register the two bridge-native meta tools.
        let registry = Registry::with_builtins();
        for route in generate_build_routes(&registry, &caps) {
            router.add_route(route);
        }
        // win32_list_toolchains: always advertised, read-only (DiscoveryToolIsReadOnly).
        router.add_route(list_toolchains_route());
        // win32_register_toolchain: advertised only under the operator opt-in
        // (RegistrationRequiresOptIn).
        if caps.satisfies("toolchain_registration") {
            router.add_route(register_toolchain_route());
        }

        Bridge {
            inner: Arc::new(Inner {
                caps,
                device: Mutex::new(device),
                counter: AtomicU64::new(1),
                registry: Mutex::new(registry),
                breaker: Mutex::new(breaker),
                limiter: Mutex::new(limiter),
                audit,
            }),
            tool_router: router,
        }
    }

    fn next_id(&self) -> String {
        let n = self.inner.counter.fetch_add(1, Ordering::Relaxed);
        format!("b{n}")
    }

    /// Round-trip a command to the device through the resilience path and map
    /// the reply. `tool_name` is the MCP tool the call came from (the rate-limit
    /// bucket key and the audit subject); `is_power` is whether this is a
    /// destructive/power tool (audited on every outcome — PowerToolsAreAudited).
    ///
    /// The order is rate-limit -> circuit-breaker -> relay (BridgeResilience):
    /// an exhausted rate cap sheds locally ("rate limited", ToolCallRateLimited)
    /// and a tripped breaker short-circuits locally ("device circuit open",
    /// ToolCallCircuitOpen) — both BEFORE the device is touched. Otherwise the
    /// call relays: ok -> structuredContent; a device `status:"error"` or a
    /// transport failure -> a recoverable isError (the breaker counts the
    /// failure). The audit record carries the relayed wire args, redacted.
    async fn dispatch(&self, tool_name: &str, cmd: Command, is_power: bool) -> CallToolResult {
        // (1) Rate-limit. An exhausted bucket sheds the call locally.
        if !self.inner.limiter.lock().await.try_acquire(tool_name) {
            if is_power {
                self.inner
                    .audit
                    .record(tool_name, &cmd.args, Outcome::RateLimited);
            }
            return CallToolResult::error(vec![Content::text("rate limited")]);
        }

        // (2) Circuit-breaker. A tripped breaker short-circuits locally.
        if !self.inner.breaker.lock().await.admit() {
            if is_power {
                self.inner
                    .audit
                    .record(tool_name, &cmd.args, Outcome::DeviceCircuitOpen);
            }
            return CallToolResult::error(vec![Content::text("device circuit open")]);
        }

        // (3) Relay to the device.
        let result = {
            let mut dev = self.inner.device.lock().await;
            dev.call(&cmd).await
        };
        match result {
            // ok -> structuredContent (rmcp's `structured` also mirrors the
            // JSON into a text content block for clients that only read text).
            Ok(resp) if resp.is_ok() => {
                self.inner.breaker.lock().await.record_success();
                if is_power {
                    self.inner
                        .audit
                        .record(tool_name, &cmd.args, Outcome::Relayed);
                }
                CallToolResult::structured(json!(resp.fields))
            }
            // A device status:"error" is a recoverable tool error (isError),
            // and a device-side fault that counts toward the breaker.
            Ok(resp) => {
                self.inner.breaker.lock().await.record_failure();
                if is_power {
                    self.inner
                        .audit
                        .record(tool_name, &cmd.args, Outcome::DeviceError);
                }
                let reason = resp.error.unwrap_or_else(|| "device error".to_string());
                CallToolResult::error(vec![Content::text(reason)])
            }
            // A transport failure is also recoverable from the model's view.
            Err(e) => {
                self.inner.breaker.lock().await.record_failure();
                if is_power {
                    self.inner
                        .audit
                        .record(tool_name, &cmd.args, Outcome::DeviceError);
                }
                CallToolResult::error(vec![Content::text(format!("device unreachable: {e}"))])
            }
        }
    }

    /// A non-power relay (read-only/non-destructive tool): the resilience path
    /// without an audit record (PowerToolsAreAudited audits only power tools;
    /// read-only tools like win32_list_commands produce no audit record).
    async fn round_trip(&self, tool_name: &str, cmd: Command) -> CallToolResult {
        self.dispatch(tool_name, cmd, false).await
    }

    /// A power/destructive relay: the resilience path WITH an audit record on
    /// every outcome path (PowerToolsAreAudited).
    async fn round_trip_power(&self, tool_name: &str, cmd: Command) -> CallToolResult {
        self.dispatch(tool_name, cmd, true).await
    }

    /// Run a generated build tool: typed params -> injection-safe argv
    /// (`BuildArgvIsCatalogued`) -> a catalogued `exec` on the device -> parse
    /// the diagnostic output into a structured `BuildOutcome`. A nonzero
    /// compiler exit is NOT a tool error — it rides the structuredContent of a
    /// successful call (compile-error-≠-tool-error). The device failing to RUN
    /// the toolchain (a `status:"error"` reply) is the only isError case.
    // The build descriptors (role/command/template/dialect/diagnostic) are a
    // flat per-call list bound in build_route; the audit added tool_name,
    // tipping one past clippy's 7-arg heuristic. Bundling them into a struct
    // would be a single-use wrapper, so the list is kept flat.
    #[allow(clippy::too_many_arguments)]
    async fn run_build(
        &self,
        tool_name: &str,
        role: ToolRole,
        command: String,
        template: Vec<ArgItem>,
        dialect: String,
        diagnostic: DiagnosticSpec,
        args: Map<String, Value>,
    ) -> CallToolResult {
        let params = match argv::parse_params(role, &args) {
            Ok(p) => p,
            Err(e) => {
                return CallToolResult::error(vec![Content::text(format!(
                    "invalid build parameters: {e}"
                ))])
            }
        };
        let argv = match argv::emit_argv(&command, &template, &params) {
            Ok(a) => a,
            Err(e) => {
                return CallToolResult::error(vec![Content::text(format!(
                    "could not build the command line: {e}"
                ))])
            }
        };

        // argv[0] is the role's catalogued command; the device re-gates it
        // (CatalogLookup + CatalogValidateArgs) — the BuildArgvIsCatalogued
        // backstop. A build tool is a POWER tool (it writes/overwrites .obj/
        // .exe/.lib), so its relay runs through the resilience path
        // (rate-limit -> breaker) and is audited on every outcome.
        let cmd = Command::new("exec", self.next_id())
            .with("argv", json!(argv))
            .with("timeout_ms", json!(BUILD_TIMEOUT_MS));

        // (1) Rate-limit.
        if !self.inner.limiter.lock().await.try_acquire(tool_name) {
            self.inner
                .audit
                .record(tool_name, &cmd.args, Outcome::RateLimited);
            return CallToolResult::error(vec![Content::text("rate limited")]);
        }
        // (2) Circuit-breaker.
        if !self.inner.breaker.lock().await.admit() {
            self.inner
                .audit
                .record(tool_name, &cmd.args, Outcome::DeviceCircuitOpen);
            return CallToolResult::error(vec![Content::text("device circuit open")]);
        }
        // (3) Relay.
        let result = {
            let mut dev = self.inner.device.lock().await;
            dev.call(&cmd).await
        };
        let resp = match result {
            Ok(r) => r,
            Err(e) => {
                self.inner.breaker.lock().await.record_failure();
                self.inner
                    .audit
                    .record(tool_name, &cmd.args, Outcome::DeviceError);
                return CallToolResult::error(vec![Content::text(format!(
                    "device unreachable: {e}"
                ))]);
            }
        };
        if !resp.is_ok() {
            // The toolchain failed to *run* (catalog reject, spawn failure,
            // timeout) — the recoverable isError case.
            self.inner.breaker.lock().await.record_failure();
            self.inner
                .audit
                .record(tool_name, &cmd.args, Outcome::DeviceError);
            let reason = resp
                .error
                .unwrap_or_else(|| "the build command could not be run".to_string());
            return CallToolResult::error(vec![Content::text(reason)]);
        }
        // The toolchain RAN (regardless of compile pass/fail): a relayed
        // success from the breaker's view, audited as relayed.
        self.inner.breaker.lock().await.record_success();
        self.inner
            .audit
            .record(tool_name, &cmd.args, Outcome::Relayed);

        let exit_code = resp
            .fields
            .get("exit_code")
            .and_then(Value::as_i64)
            .unwrap_or(-1);
        let stdout = decode_b64_field(&resp.fields, "stdout_b64");
        let stderr = decode_b64_field(&resp.fields, "stderr_b64");
        let outcome =
            match diagnostics::build_outcome(&diagnostic, &dialect, exit_code, &stdout, &stderr) {
                Ok(o) => o,
                Err(e) => {
                    return CallToolResult::error(vec![Content::text(format!(
                        "could not parse build output: {e}"
                    ))])
                }
            };
        match serde_json::to_value(&outcome) {
            Ok(v) => CallToolResult::structured(v),
            Err(e) => {
                CallToolResult::error(vec![Content::text(format!("result encode error: {e}"))])
            }
        }
    }

    /// `win32_list_toolchains`: the read-only discovery tool. Returns the role
    /// parameter schemas, the loaded definitions (identity + per-role commands),
    /// and the authoring guide — everything an agent needs to author a new
    /// definition. Mutates nothing.
    async fn list_toolchains(&self) -> CallToolResult {
        let registry = self.inner.registry.lock().await;
        let definitions: Vec<Value> = registry
            .definitions()
            .iter()
            .map(|d| {
                let roles: Vec<Value> = d
                    .roles
                    .iter()
                    .map(|(role, rs)| {
                        json!({
                            "role": role.as_str(),
                            "command": rs.command,
                            "tool": build_tool_name(&d.name, *role),
                        })
                    })
                    .collect();
                json!({
                    "name": d.name,
                    "vendor": d.vendor,
                    "source": source_token(d.source),
                    "supported_versions": d.supported_versions,
                    "roles": roles,
                })
            })
            .collect();
        let schemas = json!({
            "compile": argv::role_schema(ToolRole::Compile),
            "link": argv::role_schema(ToolRole::Link),
            "lib": argv::role_schema(ToolRole::Lib),
            "assemble": argv::role_schema(ToolRole::Assemble),
        });
        CallToolResult::structured(json!({
            "definitions": definitions,
            "role_schemas": schemas,
            "guide": TOOLCHAIN_GUIDE,
        }))
    }

    /// `win32_register_toolchain`: the opt-in runtime registration tool. Parses
    /// the submitted definition, pins it `registered`, re-validates that every
    /// command it drives is catalogued (`DefinitionCommandsAreCatalogued` —
    /// authorship never escapes the device gate), and adds it to the registry.
    /// The generated build tools materialise on the next session.
    async fn register_toolchain(&self, args: Map<String, Value>) -> CallToolResult {
        let definition_value = match args.get("definition") {
            Some(v) => v.clone(),
            None => {
                return CallToolResult::error(vec![Content::text(
                    "missing `definition` (the ToolchainDefinition to register)".to_string(),
                )])
            }
        };
        let def = match definition::from_json(&definition_value, DefinitionSource::Registered) {
            Ok(d) => d,
            Err(e) => {
                return CallToolResult::error(vec![Content::text(format!(
                    "invalid toolchain definition: {e}"
                ))])
            }
        };

        let mut registry = self.inner.registry.lock().await;
        let mut candidate = registry.clone();
        candidate.add(def.clone());
        // The catalogued-command floor: a registered definition may only drive
        // commands already on the device allow-list (the built-ins' commands).
        if let Err(e) =
            candidate.validate_catalogued(|cmd| catalogued_build_commands().contains(cmd))
        {
            return CallToolResult::error(vec![Content::text(format!(
                "registration refused: {e}"
            ))]);
        }
        registry.add(def.clone());
        let commands: Vec<&str> = def.commands();
        CallToolResult::structured(json!({
            "registered": true,
            "name": def.name,
            "vendor": def.vendor,
            "commands": commands,
        }))
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
        Ok(self.round_trip("win32_echo", cmd).await)
    }

    /// Read a file from the Win32 host. 1:1 relay to the device `read`
    /// command (path -> path).
    #[tool(
        name = "win32_read_file",
        description = "Read a file from the Win32 host; returns its contents (base64 for binary safety).",
        annotations(read_only_hint = true)
    )]
    pub async fn win32_read_file(
        &self,
        Parameters(p): Parameters<PathParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("read", self.next_id()).with("path", Value::String(p.path));
        Ok(self.round_trip("win32_read_file", cmd).await)
    }

    /// Write a file on the Win32 host. 1:1 relay to the device `write`
    /// command (path -> path, data -> data). `data` is base64.
    #[tool(
        name = "win32_write_file",
        description = "Write a file on the Win32 host. `data` must be base64-encoded (the device wire is base64 for binary safety; pass the raw bytes encoded, not transcoded text).",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_write_file(
        &self,
        Parameters(p): Parameters<WriteParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("write", self.next_id())
            .with("path", Value::String(p.path))
            .with("data", Value::String(p.data));
        Ok(self.round_trip_power("win32_write_file", cmd).await)
    }

    /// List a directory on the Win32 host. 1:1 relay to the device `list`
    /// command (path -> path).
    #[tool(
        name = "win32_list_dir",
        description = "List the entries of a directory on the Win32 host.",
        annotations(read_only_hint = true)
    )]
    pub async fn win32_list_dir(
        &self,
        Parameters(p): Parameters<PathParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("list", self.next_id()).with("path", Value::String(p.path));
        Ok(self.round_trip("win32_list_dir", cmd).await)
    }

    /// Delete a file on the Win32 host. 1:1 relay to the device `delete`
    /// command (path -> path).
    #[tool(
        name = "win32_delete_file",
        description = "Delete a file on the Win32 host.",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_delete_file(
        &self,
        Parameters(p): Parameters<PathParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("delete", self.next_id()).with("path", Value::String(p.path));
        Ok(self.round_trip_power("win32_delete_file", cmd).await)
    }

    /// Copy a file on the Win32 host. 1:1 relay to the device `copy`
    /// command (source -> path, dest -> dest). Never overwrites — it
    /// carries no destructiveHint for that reason.
    #[tool(
        name = "win32_copy_file",
        description = "Copy a file on the Win32 host. Fails if `dest` already exists — it never overwrites; to replace a file, delete it first then copy."
    )]
    pub async fn win32_copy_file(
        &self,
        Parameters(p): Parameters<SourceDestParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("copy", self.next_id())
            .with("path", Value::String(p.source))
            .with("dest", Value::String(p.dest));
        Ok(self.round_trip("win32_copy_file", cmd).await)
    }

    /// Move/rename a file on the Win32 host. 1:1 relay to the device
    /// `move` command (source -> path, dest -> dest).
    #[tool(
        name = "win32_move_file",
        description = "Move or rename a file on the Win32 host. Fails if `dest` already exists.",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_move_file(
        &self,
        Parameters(p): Parameters<SourceDestParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("move", self.next_id())
            .with("path", Value::String(p.source))
            .with("dest", Value::String(p.dest));
        Ok(self.round_trip_power("win32_move_file", cmd).await)
    }

    /// Create a directory on the Win32 host. 1:1 relay to the device
    /// `mkdir` command (path -> path).
    #[tool(
        name = "win32_make_dir",
        description = "Create a single directory level on the Win32 host. Does not create missing parents — a missing parent errors \"path not found\"; create parents first."
    )]
    pub async fn win32_make_dir(
        &self,
        Parameters(p): Parameters<PathParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("mkdir", self.next_id()).with("path", Value::String(p.path));
        Ok(self.round_trip("win32_make_dir", cmd).await)
    }

    /// Remove an empty directory on the Win32 host. 1:1 relay to the
    /// device `rmdir` command (path -> path).
    #[tool(
        name = "win32_remove_dir",
        description = "Remove a directory on the Win32 host. The directory must be empty — it does not delete recursively; a non-empty directory errors \"directory not empty\".",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_remove_dir(
        &self,
        Parameters(p): Parameters<PathParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("rmdir", self.next_id()).with("path", Value::String(p.path));
        Ok(self.round_trip_power("win32_remove_dir", cmd).await)
    }

    // ----- memory tools -------------------------------------------------
    // Each is a 1:1 relay to a device wire command (MemoryToolsRegistered).
    // They are gated by GATED_TOOLS: spawn_retain/peek/terminate/release on
    // "mem", poke on the two-factor "mem_write" — pruned from tools/list on a
    // device that lacks the tier / the operator opt-in (the first real G1
    // prune). Safety is enforced device-side (the catalog gate, the range/
    // region guards, the /ALLOWMEMWRITE arm, the audit log); these hints are
    // advisory only (MemoryToolHintsAreHonest).

    /// Launch a process and retain it as a memory target, returning an opaque
    /// token. 1:1 relay to the device `spawnRetain` command. You MUST call
    /// `win32_release` (or `win32_terminate`) on the token when done.
    #[tool(
        name = "win32_spawn_retain",
        description = "Launch a catalogued command and retain its process as a memory target (NT+). Returns a token for win32_peek/win32_poke. Call win32_release or win32_terminate when finished — retained processes are bounded.",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_spawn_retain(
        &self,
        Parameters(p): Parameters<SpawnRetainParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let mut cmd = Command::new("spawnRetain", self.next_id()).with("argv", json!(p.argv));
        if let Some(cwd) = p.cwd {
            cmd = cmd.with("cwd", Value::String(cwd));
        }
        Ok(self.round_trip_power("win32_spawn_retain", cmd).await)
    }

    /// Read memory from a retained process (or the shared address space on
    /// pre-NT). 1:1 relay to the device `peek` command (token -> mem_token,
    /// addr -> mem_addr, len -> mem_len). Returns the bytes base64-encoded.
    #[tool(
        name = "win32_peek",
        description = "Read up to 65536 bytes of memory. On NT+ pass the `token` from win32_spawn_retain; `addr` is a hex (0x...) or decimal string. Returns base64 bytes; a pre-NT read may be truncated at a non-accessible region.",
        annotations(read_only_hint = true)
    )]
    pub async fn win32_peek(
        &self,
        Parameters(p): Parameters<PeekParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let mut cmd = Command::new("peek", self.next_id())
            .with("mem_addr", Value::String(p.addr))
            .with("mem_len", Value::String(p.len.to_string()));
        if let Some(token) = p.token {
            cmd = cmd.with("mem_token", Value::String(token));
        }
        Ok(self.round_trip("win32_peek", cmd).await)
    }

    /// Write memory to a retained process (or the shared address space on
    /// pre-NT). 1:1 relay to the device `poke` command (token -> mem_token,
    /// addr -> mem_addr, data -> data). Gated: requires `--allow-memory-write`
    /// AND the device `/ALLOWMEMWRITE` arm; every write is audit-logged.
    #[tool(
        name = "win32_poke",
        description = "Write base64-encoded bytes to memory (at most 65536). On NT+ pass the `token` from win32_spawn_retain; `addr` is a hex (0x...) or decimal string. Off by default — the operator must arm both the bridge and the device; every poke is audit-logged device-side.",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_poke(
        &self,
        Parameters(p): Parameters<PokeParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let mut cmd = Command::new("poke", self.next_id())
            .with("mem_addr", Value::String(p.addr))
            .with("data", Value::String(p.data));
        if let Some(token) = p.token {
            cmd = cmd.with("mem_token", Value::String(token));
        }
        Ok(self.round_trip_power("win32_poke", cmd).await)
    }

    /// Terminate a retained process and free its slot. 1:1 relay to the device
    /// `terminate` command (token -> mem_token). The token is consumed.
    #[tool(
        name = "win32_terminate",
        description = "Terminate a retained process (TerminateProcess) and free its slot. The token is consumed and cannot be reused.",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_terminate(
        &self,
        Parameters(p): Parameters<TokenParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd =
            Command::new("terminate", self.next_id()).with("mem_token", Value::String(p.token));
        Ok(self.round_trip_power("win32_terminate", cmd).await)
    }

    /// Release a retained process handle WITHOUT killing the child (it keeps
    /// running). 1:1 relay to the device `release` command (token -> mem_token).
    /// The token is consumed.
    #[tool(
        name = "win32_release",
        description = "Release a retained process handle without killing the child — it keeps running, but the token is consumed and can no longer be peeked/poked."
    )]
    pub async fn win32_release(
        &self,
        Parameters(p): Parameters<TokenParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("release", self.next_id()).with("mem_token", Value::String(p.token));
        Ok(self.round_trip("win32_release", cmd).await)
    }

    // ----- exec discovery tools -----------------------------------------
    // The three exec tools are 1:1 relays to the device's exec/ptyExec/
    // listCommands wire commands (ExecToolsRegistered, ExecToolsAreDirectRelays).
    // win32_exec/win32_pty_exec are destructive and power-audited; win32_exec
    // carries the per-call unsafe-bypass gate (UnsafeExecRequiresOperatorOptIn);
    // win32_pty_exec is capability-gated on "pty" via GATED_TOOLS
    // (PtyToolGatedOnCapability). win32_list_commands is read-only and always
    // advertised (no required capability), like win32_list_toolchains.

    /// Run a catalogued command on the Win32 host, capturing stdout/stderr.
    /// 1:1 relay to the device `exec` command. THE UNSAFE-EXEC SAFETY GATE
    /// (UnsafeExecRequiresOperatorOptIn): `unsafe:true` requests the device
    /// catalog bypass; the bridge relays it ONLY when the operator passed
    /// `--allow-unsafe-exec`, otherwise it refuses the call locally and the
    /// device receives nothing.
    #[tool(
        name = "win32_exec",
        description = "Run a catalogued command on the Win32 host, capturing stdout/stderr. `argv[0]` must be on the device command catalog. `unsafe:true` bypasses the catalog allow-list and is refused unless the operator armed the bridge with --allow-unsafe-exec.",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_exec(
        &self,
        Parameters(p): Parameters<ExecParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let unsafe_requested = p.unsafe_bypass == Some(true);
        // THE SAFETY PIN (ExecUnsafeRejected / UnsafeExecRequiresOperatorOptIn):
        // the per-call decision is the pure `unsafe_gate` (so the property test
        // exercises the exact branch the handler takes). RefuseLocally → an
        // unsafe-bypass exec without the operator opt-in is refused LOCALLY: a
        // recoverable isError, the device is NEVER touched, and the power audit
        // records the refusal (outcome unsafe_opt_in_absent).
        if unsafe_gate(unsafe_requested, self.inner.caps.allow_unsafe_exec)
            == ExecGate::RefuseLocally
        {
            self.inner.audit.record(
                "win32_exec",
                &exec_audit_args(&p),
                Outcome::UnsafeOptInAbsent,
            );
            return Ok(CallToolResult::error(vec![Content::text(
                "unsafe exec not permitted: operator opt-in required",
            )]));
        }

        let mut cmd = Command::new("exec", self.next_id()).with("argv", json!(p.argv));
        if let Some(cwd) = p.cwd {
            cmd = cmd.with("cwd", Value::String(cwd));
        }
        if let Some(timeout_ms) = p.timeout_ms {
            cmd = cmd.with("timeout_ms", json!(timeout_ms));
        }
        if let Some(stdin_b64) = p.stdin_b64 {
            cmd = cmd.with("stdin_b64", Value::String(stdin_b64));
        }
        if let Some(max_output) = p.max_output {
            cmd = cmd.with("max_output", json!(max_output));
        }
        if let Some(shell) = p.shell {
            cmd = cmd.with("shell", Value::Bool(shell));
        }
        if let Some(mem_cap_bytes) = p.mem_cap_bytes {
            cmd = cmd.with("mem_cap_bytes", json!(mem_cap_bytes));
        }
        if let Some(cpu_time_ms) = p.cpu_time_ms {
            cmd = cmd.with("cpu_time_ms", json!(cpu_time_ms));
        }
        // Relay `unsafe:true` ONLY when the bypass was requested (and, by the
        // gate above, the operator opted in). A plain exec carries no `unsafe`.
        if unsafe_requested {
            cmd = cmd.with("unsafe", Value::Bool(true));
        }
        Ok(self.round_trip_power("win32_exec", cmd).await)
    }

    /// Run a catalogued command under a pseudo-console (ConPTY, Win10 1809+),
    /// capturing the combined terminal stream. 1:1 relay to the device
    /// `ptyExec` command. Capability-gated on "pty": advertised only on a
    /// ConPTY-capable device (PtyToolGatedOnCapability).
    #[tool(
        name = "win32_pty_exec",
        description = "Run a catalogued command under a pseudo-console (ConPTY) on the Win32 host, capturing the combined terminal stream. Available only on a ConPTY-capable device (Windows 10 1809+).",
        annotations(destructive_hint = true)
    )]
    pub async fn win32_pty_exec(
        &self,
        Parameters(p): Parameters<PtyExecParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let mut cmd = Command::new("ptyExec", self.next_id()).with("argv", json!(p.argv));
        if let Some(cwd) = p.cwd {
            cmd = cmd.with("cwd", Value::String(cwd));
        }
        if let Some(timeout_ms) = p.timeout_ms {
            cmd = cmd.with("timeout_ms", json!(timeout_ms));
        }
        if let Some(stdin_b64) = p.stdin_b64 {
            cmd = cmd.with("stdin_b64", Value::String(stdin_b64));
        }
        if let Some(max_output) = p.max_output {
            cmd = cmd.with("max_output", json!(max_output));
        }
        if let Some(shell) = p.shell {
            cmd = cmd.with("shell", Value::Bool(shell));
        }
        if let Some(cols) = p.cols {
            cmd = cmd.with("cols", json!(cols));
        }
        if let Some(rows) = p.rows {
            cmd = cmd.with("rows", json!(rows));
        }
        Ok(self.round_trip_power("win32_pty_exec", cmd).await)
    }

    /// List the commands on the device's catalog allow-list. 1:1 relay to the
    /// device `listCommands` command. Read-only and always advertised (no
    /// required capability), like win32_list_toolchains.
    #[tool(
        name = "win32_list_commands",
        description = "List the commands on the device's catalog allow-list (the programs win32_exec/win32_pty_exec may run). Read-only — it reports the catalog and changes nothing.",
        annotations(read_only_hint = true)
    )]
    pub async fn win32_list_commands(
        &self,
        Parameters(_p): Parameters<ListCommandsParams>,
    ) -> Result<CallToolResult, ErrorData> {
        let cmd = Command::new("listCommands", self.next_id());
        Ok(self.round_trip("win32_list_commands", cmd).await)
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

// ------------------------------------------------------------------
// The build subsystem seam: dynamic tool generation from definitions
// ------------------------------------------------------------------

/// The per-call decision for `win32_exec`'s unsafe-bypass gate
/// (UnsafeExecRequiresOperatorOptIn): whether to relay to the device or refuse
/// locally. `Relay` covers both a plain exec and an opted-in unsafe exec;
/// `RefuseLocally` is the safety pin's refusal (an unsafe request without the
/// operator opt-in — the device receives nothing).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExecGate {
    Relay,
    RefuseLocally,
}

/// THE SAFETY PIN, as a pure function (so the property test and the handler
/// share one branch): an exec relays UNLESS it requested the unsafe catalog
/// bypass and the operator did not opt in. `unsafe_requested ∧ ¬opt_in ⟹
/// RefuseLocally`; otherwise `Relay`. Mirrors the spec's
/// `not unsafe_requested or caps.allow_unsafe_exec` dispatch precondition
/// (ToolCallDispatched) and its negation (ExecUnsafeRejected).
pub fn unsafe_gate(unsafe_requested: bool, allow_unsafe_exec: bool) -> ExecGate {
    if unsafe_requested && !allow_unsafe_exec {
        ExecGate::RefuseLocally
    } else {
        ExecGate::Relay
    }
}

/// The argument summary for an exec call's audit record on the local-refusal
/// path (`unsafe_opt_in_absent`), where the wire `Command` is never built. The
/// device never receives these — they are only what the operator authorised the
/// model to ATTEMPT — so the audit captures the argv and the unsafe request.
fn exec_audit_args(p: &ExecParams) -> Map<String, Value> {
    let mut args = Map::new();
    args.insert("argv".to_string(), json!(p.argv));
    if let Some(cwd) = &p.cwd {
        args.insert("cwd".to_string(), Value::String(cwd.clone()));
    }
    args.insert(
        "unsafe".to_string(),
        Value::Bool(p.unsafe_bypass == Some(true)),
    );
    args
}

/// The generated tool name for a (definition, role): `win32_<name>_<role>`
/// (e.g. `win32_msvc_compile`, `win32_watcom_link`). The spec's
/// `build_tool_name` black box.
fn build_tool_name(def_name: &str, role: ToolRole) -> String {
    format!("win32_{}_{}", def_name, role.as_str())
}

/// The diagnostic dialect label carried on the `BuildOutcome`: `<name>_cc` for
/// compile/assemble, `<name>_link` for link/lib — the four built-in dialects
/// (`msvc_cc`, `msvc_link`, `watcom_cc`, `watcom_link`) fall out of this.
fn dialect_name(def_name: &str, role: ToolRole) -> String {
    let group = match role {
        ToolRole::Compile | ToolRole::Assemble => "cc",
        ToolRole::Link | ToolRole::Lib => "link",
    };
    format!("{def_name}_{group}")
}

/// The serde token for a definition's source (matches the wire/JSON form).
fn source_token(source: DefinitionSource) -> &'static str {
    match source {
        DefinitionSource::BuiltIn => "built_in",
        DefinitionSource::Config => "config",
        DefinitionSource::Registered => "registered",
    }
}

/// The build commands the device catalog is known to carry: every command the
/// built-in definitions drive (we add exactly these eight catalog entries). The
/// `is_catalogued` oracle for runtime registration — a registered definition
/// may only re-describe already-allow-listed commands, never introduce a new
/// executable (`DefinitionCommandsAreCatalogued`).
fn catalogued_build_commands() -> HashSet<String> {
    definition::builtins()
        .iter()
        .flat_map(|d| d.commands().into_iter().map(String::from))
        .collect()
}

/// Base64-decode a device reply field (`stdout_b64`/`stderr_b64`) to text. The
/// device owns the whole text pipeline and transcodes console output
/// to UTF-8 on every tier, so these bytes are ALREADY valid UTF-8 — the bridge
/// only validates them (no codepage logic, no `encoding_rs`/`oem_cp`). The
/// `from_utf8_lossy` is kept purely as a belt-and-suspenders for a malformed
/// reply: a conformant device never triggers a replacement here.
fn decode_b64_field(fields: &Map<String, Value>, key: &str) -> String {
    let b64 = fields.get(key).and_then(Value::as_str).unwrap_or("");
    match base64::engine::general_purpose::STANDARD.decode(b64) {
        Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
        Err(_) => String::new(),
    }
}

/// Generate one build-tool route per supported (definition, role): a detected
/// toolchain whose driver/probe command matches a definition, and whose version
/// the support matrix accepts, yields a tool for every role that definition
/// implements (`BuildToolGenerated`). Each definition is generated at most once
/// even if several detected commands map to it.
fn generate_build_routes(registry: &Registry, caps: &Capabilities) -> Vec<ToolRoute<Bridge>> {
    let mut routes = Vec::new();
    let mut done: HashSet<String> = HashSet::new();
    for tc in &caps.toolchains {
        for def in registry.definitions() {
            if done.contains(&def.name) {
                continue;
            }
            let matches = def.version_probe.command == tc.command
                || def.roles.values().any(|r| r.command == tc.command);
            if !(matches && def.supports(&tc.version)) {
                continue;
            }
            done.insert(def.name.clone());
            for (role, rs) in &def.roles {
                routes.push(build_route(&def.name, *role, rs));
            }
        }
    }
    routes
}

/// One generated build tool. `device_cmd` is null (it is composed of a
/// catalogued exec plus diagnostic parsing, not a 1:1 relay); the hints are
/// honest — a build writes/overwrites artefacts, so `destructive`,
/// `read_only:false`.
fn build_route(def_name: &str, role: ToolRole, rs: &RoleSpec) -> ToolRoute<Bridge> {
    let name = build_tool_name(def_name, role);
    let dialect = dialect_name(def_name, role);
    let description = format!(
        "Run the {role} step of the {def_name} toolchain. Diagnostics are returned as structured \
         data; a nonzero compiler exit is a build failure (success:false), not a tool error.",
        role = role.as_str(),
    );
    let tool = Tool::new(name.clone(), description, object(argv::role_schema(role)))
        .annotate(ToolAnnotations::new().read_only(false).destructive(true));

    let command = rs.command.clone();
    let template = rs.args.clone();
    let diagnostic = rs.diagnostic.clone();
    ToolRoute::new_dyn(tool, move |ctx: ToolCallContext<'_, Bridge>| {
        let bridge = ctx.service.clone();
        let args = ctx.arguments.clone().unwrap_or_default();
        let tool_name = name.clone();
        let command = command.clone();
        let template = template.clone();
        let dialect = dialect.clone();
        let diagnostic = diagnostic.clone();
        Box::pin(async move {
            Ok(bridge
                .run_build(
                    &tool_name, role, command, template, dialect, diagnostic, args,
                )
                .await)
        })
    })
}

/// The always-advertised read-only discovery tool route.
fn list_toolchains_route() -> ToolRoute<Bridge> {
    let schema = json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": {},
        "additionalProperties": false
    });
    let tool = Tool::new(
        "win32_list_toolchains",
        "List the build toolchains available to author against: the role parameter schemas, the \
         loaded definitions, and the authoring guide. Read-only — it returns information and \
         changes nothing.",
        object(schema),
    )
    .annotate(ToolAnnotations::new().read_only(true).destructive(false));
    ToolRoute::new_dyn(tool, |ctx: ToolCallContext<'_, Bridge>| {
        let bridge = ctx.service.clone();
        Box::pin(async move { Ok(bridge.list_toolchains().await) })
    })
}

/// The opt-in runtime-registration tool route (only added when the operator
/// enabled `toolchain_registration`).
fn register_toolchain_route() -> ToolRoute<Bridge> {
    let schema = json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": {
            "definition": {
                "type": "object",
                "description": "A ToolchainDefinition (name, vendor, version_probe, supported_versions, roles). Every command it drives must already be on the device command catalog."
            }
        },
        "required": ["definition"],
        "additionalProperties": false
    });
    let tool = Tool::new(
        "win32_register_toolchain",
        "Register a new toolchain definition at runtime (operator opt-in). The definition may only \
         drive catalogued commands; its generated build tools appear on the next session.",
        object(schema),
    )
    .annotate(ToolAnnotations::new().read_only(false).destructive(false));
    ToolRoute::new_dyn(tool, |ctx: ToolCallContext<'_, Bridge>| {
        let bridge = ctx.service.clone();
        let args = ctx.arguments.clone().unwrap_or_default();
        Box::pin(async move { Ok(bridge.register_toolchain(args).await) })
    })
}

#[cfg(test)]
mod decode_tests {
    use super::*;
    use base64::Engine;

    /// passthrough_validates_utf8 (OBLIGATIONS-5.4.md, bridge): the device now
    /// guarantees UTF-8, so base64 of valid UTF-8 bytes (CJK here) round-trips
    /// through decode_b64_field byte-for-byte — no codepage transcoding, no
    /// replacement. This is the bridge passthrough contract.
    #[test]
    fn passthrough_validates_utf8() {
        // "日本語" — U+65E5 U+672C U+8A9E, the device's UTF-8 wire bytes.
        let utf8 = "日本語";
        let b64 = base64::engine::general_purpose::STANDARD.encode(utf8.as_bytes());
        let mut fields = Map::new();
        fields.insert("stdout_b64".to_string(), Value::String(b64));
        let got = decode_b64_field(&fields, "stdout_b64");
        assert_eq!(got, utf8, "valid UTF-8 passes through unchanged");
        assert!(!got.contains('\u{FFFD}'), "no replacement char introduced");
    }
}
