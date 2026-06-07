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

use crate::capabilities::{tools_to_prune, Capabilities};
use crate::device::Device;
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

struct Inner {
    caps: Capabilities,
    device: Mutex<Box<dyn Device>>,
    counter: AtomicU64,
    /// The loaded toolchain definitions (built-ins + any runtime-registered).
    /// Build-tool routes are generated from this at construction; runtime
    /// registration mutates it (the new tools materialise on the next session,
    /// per the registry-as-source-of-truth model).
    registry: Mutex<Registry>,
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

    /// Run a generated build tool: typed params -> injection-safe argv
    /// (`BuildArgvIsCatalogued`) -> a catalogued `exec` on the device -> parse
    /// the diagnostic output into a structured `BuildOutcome`. A nonzero
    /// compiler exit is NOT a tool error — it rides the structuredContent of a
    /// successful call (compile-error-≠-tool-error). The device failing to RUN
    /// the toolchain (a `status:"error"` reply) is the only isError case.
    async fn run_build(
        &self,
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
        // backstop. This is the bridge's first `exec` user.
        let cmd = Command::new("exec", self.next_id())
            .with("argv", json!(argv))
            .with("timeout_ms", json!(BUILD_TIMEOUT_MS));
        let result = {
            let mut dev = self.inner.device.lock().await;
            dev.call(&cmd).await
        };
        let resp = match result {
            Ok(r) => r,
            Err(e) => {
                return CallToolResult::error(vec![Content::text(format!(
                    "device unreachable: {e}"
                ))])
            }
        };
        if !resp.is_ok() {
            // The toolchain failed to *run* (catalog reject, spawn failure,
            // timeout) — the recoverable isError case.
            let reason = resp
                .error
                .unwrap_or_else(|| "the build command could not be run".to_string());
            return CallToolResult::error(vec![Content::text(reason)]);
        }

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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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
        Ok(self.round_trip(cmd).await)
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

// ------------------------------------------------------------------
// The build subsystem seam: dynamic tool generation from definitions
// ------------------------------------------------------------------

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

/// Base64-decode a device reply field (`stdout_b64`/`stderr_b64`) to text. 5.2
/// uses `from_utf8_lossy`; full codepage transcoding lands in 5.4.
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
    let tool = Tool::new(name, description, object(argv::role_schema(role)))
        .annotate(ToolAnnotations::new().read_only(false).destructive(true));

    let command = rs.command.clone();
    let template = rs.args.clone();
    let diagnostic = rs.diagnostic.clone();
    ToolRoute::new_dyn(tool, move |ctx: ToolCallContext<'_, Bridge>| {
        let bridge = ctx.service.clone();
        let args = ctx.arguments.clone().unwrap_or_default();
        let command = command.clone();
        let template = template.clone();
        let dialect = dialect.clone();
        let diagnostic = diagnostic.clone();
        Box::pin(async move {
            Ok(bridge
                .run_build(role, command, template, dialect, diagnostic, args)
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
