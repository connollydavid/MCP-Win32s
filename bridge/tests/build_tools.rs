//! 5.2 integration tests: the schema-driven build subsystem — generated
//! win32_<vendor>_<role> tools, the two meta tools, the opt-in gate, and the
//! end-to-end build pipeline (typed params -> injection-safe argv -> catalogued
//! exec -> diagnostic parse). Each cites its obligation IDs from
//! tests/OBLIGATIONS-5.2.md.

mod common;
use common as mock;

use mcp_w32s_bridge::capabilities::{Capabilities, EncodingMode, MemTier};
use mcp_w32s_bridge::server::Bridge;
use mcp_w32s_bridge::wire::{DetectedToolchain, Features};
use rmcp::model::{CallToolRequestParams, Tool};
use rmcp::service::RunningService;
use rmcp::{RoleClient, ServiceExt};
use serde_json::{json, Value};

fn caps(toolchains: Vec<DetectedToolchain>, registration: bool) -> Capabilities {
    Capabilities {
        has_pty: false,
        mem: MemTier::None,
        encoding: EncodingMode::Codepage,
        codepage: 437,
        version: "test".to_string(),
        toolchains,
        toolchain_registration: registration,
        allow_memory_write: false,
    }
}

/// A detected MSVC (the device probed `cl` and read its banner version).
fn msvc(version: &str) -> DetectedToolchain {
    DetectedToolchain {
        vendor: "Microsoft".to_string(),
        command: "cl".to_string(),
        version: version.to_string(),
    }
}

/// Spawn the bridge on an in-memory duplex and return a connected MCP client.
async fn serve(
    caps: Capabilities,
    device: Box<dyn mcp_w32s_bridge::device::Device>,
) -> RunningService<RoleClient, ()> {
    let (server_t, client_t) = tokio::io::duplex(16384);
    let bridge = Bridge::new(caps, device);
    tokio::spawn(async move {
        if let Ok(server) = bridge.serve(server_t).await {
            let _ = server.waiting().await;
        }
    });
    ().serve(client_t).await.expect("client up")
}

async fn list_tools(client: &RunningService<RoleClient, ()>) -> Vec<Tool> {
    client
        .list_tools(Default::default())
        .await
        .expect("list_tools")
        .tools
}

fn find<'a>(tools: &'a [Tool], name: &str) -> Option<&'a Tool> {
    tools.iter().find(|t| t.name == name)
}

/// rule-success.BuildToolGenerated, rule-entity-creation.BuildToolGenerated.1 —
/// a detected AND supported toolchain yields one win32_<vendor>_<role> tool per
/// role, the name encodes the vendor (win32_msvc_*), and the hints are honest
/// (destructive, not read-only). device_cmd:null is a bridge-internal concept;
/// at the MCP boundary it surfaces as the four tools existing with these hints.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn build_tools_generated_for_supported() {
    let client = serve(
        caps(vec![msvc("12.00.8804")], false),
        Box::new(mock::MockDevice),
    )
    .await;
    let tools = list_tools(&client).await;

    for role in ["compile", "link", "lib", "assemble"] {
        let name = format!("win32_msvc_{role}");
        let t = find(&tools, &name).unwrap_or_else(|| panic!("{name} is advertised"));
        let ann = t
            .annotations
            .as_ref()
            .unwrap_or_else(|| panic!("{name} carries hints"));
        assert_eq!(ann.destructive_hint, Some(true), "{name} is destructive");
        assert_eq!(ann.read_only_hint, Some(false), "{name} is not read-only");
        // The inputSchema is the role's closed 2020-12 object schema.
        assert_eq!(
            t.input_schema.get("additionalProperties"),
            Some(&Value::Bool(false)),
            "{name} inputSchema is closed"
        );
    }
    let _ = client.cancel().await;
}

/// rule-failure.BuildToolGenerated.2 (toolchain_supported) — a detected but
/// UNSUPPORTED version generates no build tool (the first real G1 prune). cl
/// 13.10 (VC7) is not in the support matrix, so no win32_msvc_* appears.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn unsupported_toolchain_generates_no_tool() {
    let client = serve(
        caps(vec![msvc("13.10.3077")], false),
        Box::new(mock::MockDevice),
    )
    .await;
    let tools = list_tools(&client).await;
    assert!(
        !tools.iter().any(|t| t.name.starts_with("win32_msvc_")),
        "an unsupported version generates no build tool"
    );
    let _ = client.cancel().await;
}

/// rule-success.MetaToolsRegistered — both meta tools are registered off the
/// device capabilities; tools/list shows them with object inputSchemas. (Shown
/// with the registration opt-in on, so both are advertised.)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn meta_tools_registered() {
    let client = serve(caps(vec![], true), Box::new(mock::MockDevice)).await;
    let tools = list_tools(&client).await;

    for name in ["win32_list_toolchains", "win32_register_toolchain"] {
        let t = find(&tools, name).unwrap_or_else(|| panic!("{name} is registered"));
        assert_eq!(
            t.input_schema.get("type").and_then(Value::as_str),
            Some("object"),
            "{name} has an object inputSchema"
        );
        assert_eq!(
            t.input_schema.get("additionalProperties"),
            Some(&Value::Bool(false)),
            "{name} inputSchema is closed"
        );
    }
    let _ = client.cancel().await;
}

/// invariant.DiscoveryToolIsReadOnly — win32_list_toolchains carries
/// read_only:true (it returns the schema + definitions + guide and mutates
/// nothing).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn discovery_tool_is_read_only() {
    let client = serve(caps(vec![], false), Box::new(mock::MockDevice)).await;
    let tools = list_tools(&client).await;
    let t = find(&tools, "win32_list_toolchains").expect("discovery tool advertised");
    let ann = t.annotations.as_ref().expect("hints present");
    assert_eq!(ann.read_only_hint, Some(true), "discovery is read-only");
    assert_eq!(
        ann.destructive_hint,
        Some(false),
        "discovery is not destructive"
    );
    let _ = client.cancel().await;
}

/// invariant.RegistrationRequiresOptIn — win32_register_toolchain is advertised
/// only under the operator opt-in: absent when off, present when on.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn registration_requires_opt_in() {
    let off = serve(caps(vec![], false), Box::new(mock::MockDevice)).await;
    assert!(
        find(&list_tools(&off).await, "win32_register_toolchain").is_none(),
        "registration off -> the tool is not advertised"
    );
    let _ = off.cancel().await;

    let on = serve(caps(vec![], true), Box::new(mock::MockDevice)).await;
    assert!(
        find(&list_tools(&on).await, "win32_register_toolchain").is_some(),
        "registration opted in -> the tool is advertised"
    );
    let _ = on.cancel().await;
}

/// rule-success.CapabilitiesResolved (the 5.2 additions) — from_ready parses the
/// ready features' `toolchains` array, and the operator opt-in flag threads
/// through to toolchain_registration. An empty/absent array is no toolchains.
#[test]
fn capabilities_carry_toolchain_registration() {
    let mut f = Features::default();
    f.extra.insert(
        "toolchains".to_string(),
        json!([{"vendor":"Microsoft","command":"cl","version":"12.00.8804"}]),
    );
    let c = Capabilities::from_ready(437, "t".to_string(), &f, true, false);
    assert_eq!(c.toolchains.len(), 1);
    assert_eq!(c.toolchains[0].command, "cl");
    assert_eq!(c.toolchains[0].version, "12.00.8804");
    assert!(
        c.toolchain_registration,
        "the operator opt-in threads through"
    );

    // Absent array -> no toolchains; opt-in off -> registration off.
    let c2 = Capabilities::from_ready(437, "t".to_string(), &Features::default(), false, false);
    assert!(c2.toolchains.is_empty());
    assert!(!c2.toolchain_registration);
}

/// rule-success.ToolchainRegistered — calling win32_register_toolchain with a
/// valid, fully-catalogued definition creates a `registered` definition that
/// win32_list_toolchains then reports; a definition naming an uncatalogued
/// command is refused (DefinitionCommandsAreCatalogued — authorship never
/// escapes the device gate).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn registration_creates_definition() {
    let client = serve(caps(vec![], true), Box::new(mock::MockDevice)).await;

    // A registered definition over already-catalogued commands (cl/link).
    let def = json!({
        "definition": {
            "name": "myvc",
            "vendor": "Acme",
            "version_probe": {"command": "cl", "args": [], "version_regex": "Version (?<version>\\d+\\.\\d+\\.\\d+)"},
            "supported_versions": ["12."],
            "roles": {
                "compile": {
                    "command": "cl",
                    "args": ["/c", {"each": "sources", "emit": "{}", "positional": true}],
                    "diagnostic": {"regex": "^(?<code>C\\d+): (?<message>.*)$"}
                }
            }
        }
    });
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_register_toolchain")
                .with_arguments(def.as_object().unwrap().clone()),
        )
        .await
        .expect("register call ok");
    assert_eq!(res.is_error, Some(false), "a valid registration succeeds");
    let sc = res.structured_content.expect("registration is structured");
    assert_eq!(sc.get("registered"), Some(&Value::Bool(true)));
    assert_eq!(sc.get("name").and_then(Value::as_str), Some("myvc"));

    // The discovery tool now reports the registered definition.
    let listed = client
        .call_tool(CallToolRequestParams::new("win32_list_toolchains"))
        .await
        .expect("list call ok");
    let lsc = listed.structured_content.expect("list is structured");
    let names: Vec<&str> = lsc
        .get("definitions")
        .and_then(Value::as_array)
        .unwrap()
        .iter()
        .filter_map(|d| d.get("name").and_then(Value::as_str))
        .collect();
    assert!(
        names.contains(&"myvc"),
        "the registered definition is listed: {names:?}"
    );

    // A definition naming an UNCATALOGUED command is refused.
    let rogue = json!({
        "definition": {
            "name": "rogue",
            "vendor": "Attacker",
            "version_probe": {"command": "cl", "args": [], "version_regex": "Version (?<version>\\d+)"},
            "supported_versions": ["1."],
            "roles": {
                "compile": {
                    "command": "calc",
                    "args": [],
                    "diagnostic": {"regex": "^(?<code>X\\d+): (?<message>.*)$"}
                }
            }
        }
    });
    let refused = client
        .call_tool(
            CallToolRequestParams::new("win32_register_toolchain")
                .with_arguments(rogue.as_object().unwrap().clone()),
        )
        .await
        .expect("call returns (with a tool error)");
    assert_eq!(
        refused.is_error,
        Some(true),
        "an uncatalogued command is refused"
    );

    let _ = client.cancel().await;
}

/// End-to-end build pipeline (rule-success.BuildResultProduced +
/// BuildDiagnosticRecorded, invariant.BuildArgvIsCatalogued): a build-tool call
/// drives typed params -> injection-safe argv -> catalogued exec -> structured
/// BuildOutcome. A clean compile is success:true; a nonzero compiler exit is
/// success:false but is_error:false (compile-error-≠-tool-error), with the
/// diagnostics parsed. The crafted source rides as exactly one argv element.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn build_pipeline_end_to_end() {
    // A clean compile (exit 0, no output).
    let dev = mock::BuildDevice::new(0, "", "");
    let client = serve(caps(vec![msvc("12.00.8804")], false), Box::new(dev.clone())).await;

    let args = json!({
        "sources": ["m&n.c"],
        "includes": ["inc"],
        "defines": ["WIN32=1"],
        "extra_flags": ["/O2"]
    });
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_msvc_compile")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .expect("build call ok");

    // A clean compile is a successful, structured, non-error result.
    assert_eq!(
        res.is_error,
        Some(false),
        "a clean build is not a tool error"
    );
    let sc = res.structured_content.expect("build is structured");
    assert_eq!(sc.get("success"), Some(&Value::Bool(true)));
    assert_eq!(sc.get("exit_code").and_then(Value::as_i64), Some(0));
    assert_eq!(sc.get("dialect").and_then(Value::as_str), Some("msvc_cc"));
    assert_eq!(
        sc.get("diagnostics")
            .and_then(Value::as_array)
            .map(|a| a.len()),
        Some(0)
    );

    // BuildArgvIsCatalogued, end-to-end: argv[0] is the catalogued command and
    // the crafted source rides as exactly ONE argv element (never split into a
    // second command, a flag, or a separator).
    let argv = dev.exec_argv();
    assert_eq!(argv[0], "cl", "argv[0] is the catalogued command");
    assert_eq!(
        argv,
        vec!["cl", "/nologo", "/c", "/Iinc", "/DWIN32=1", "/O2", "m&n.c"],
        "the emitted argv is exactly the template's tokens"
    );
    assert!(
        argv.contains(&"m&n.c".to_string()),
        "the crafted source is one token"
    );
    assert!(!argv.iter().any(|a| a == "n.c"), "the source never split");
    let _ = client.cancel().await;

    // A compile error: nonzero exit, an error diagnostic on stderr.
    let dev2 = mock::BuildDevice::new(2, "", "m&n.c(5): error C2065: 'x': undeclared identifier");
    let client2 = serve(caps(vec![msvc("12.00.8804")], false), Box::new(dev2)).await;
    let args2 = json!({"sources": ["m&n.c"]});
    let res2 = client2
        .call_tool(
            CallToolRequestParams::new("win32_msvc_compile")
                .with_arguments(args2.as_object().unwrap().clone()),
        )
        .await
        .expect("build call ok");

    // Compile-error-≠-tool-error: success:false, but is_error:false.
    assert_eq!(
        res2.is_error,
        Some(false),
        "a compile error is NOT a tool error"
    );
    let sc2 = res2.structured_content.expect("build is structured");
    assert_eq!(sc2.get("success"), Some(&Value::Bool(false)));
    assert_eq!(sc2.get("exit_code").and_then(Value::as_i64), Some(2));
    let diags = sc2.get("diagnostics").and_then(Value::as_array).unwrap();
    assert_eq!(diags.len(), 1, "one diagnostic parsed");
    assert_eq!(diags[0].get("code").and_then(Value::as_str), Some("C2065"));
    let _ = client2.cancel().await;
}

/// A build whose exec fails to RUN (the device rejects argv[0] / spawn fails) is
/// the one is_error:true case — a recoverable tool error, not a panic.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn build_exec_failure_is_iserror() {
    let client = serve(
        caps(vec![msvc("12.00.8804")], false),
        Box::new(mock::RecordingDevice::error("command not in catalog")),
    )
    .await;
    let args = json!({"sources": ["a.c"]});
    let res = client
        .call_tool(
            CallToolRequestParams::new("win32_msvc_compile")
                .with_arguments(args.as_object().unwrap().clone()),
        )
        .await
        .expect("call returns");
    assert_eq!(
        res.is_error,
        Some(true),
        "a failed-to-run build is a tool error"
    );
    let _ = client.cancel().await;
}
