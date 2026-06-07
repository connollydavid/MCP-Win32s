//! Tests for the API-first file tool surface: the eight win32_* file
//! tools, their honest annotations, the supersession of the shell file
//! builtins, and their 1:1 relay to the device wire commands. Each test
//! cites its obligation IDs from tests/OBLIGATIONS-5.1.md ("Bridge: the
//! file tool surface").

mod common;
use common as mock;

use mcp_w32s_bridge::capabilities::{Capabilities, EncodingMode, MemTier};
use mcp_w32s_bridge::server::{Bridge, PathParams, SourceDestParams, WriteParams};
use rmcp::handler::server::wrapper::Parameters;

fn caps() -> Capabilities {
    Capabilities {
        has_pty: false,
        mem: MemTier::None,
        encoding: EncodingMode::Codepage,
        codepage: 437,
        version: "test".to_string(),
    }
}

/// The eight win32_* file tools, in registration order.
const FILE_TOOLS: &[&str] = &[
    "win32_read_file",
    "win32_write_file",
    "win32_list_dir",
    "win32_delete_file",
    "win32_copy_file",
    "win32_move_file",
    "win32_make_dir",
    "win32_remove_dir",
];

/// rule-success.FileToolsRegistered, rule-entity-creation.FileToolsRegistered.1 —
/// tools/list over the duplex advertises exactly the eight win32_* file
/// tools plus the existing 5.0 echo tool, each with an inputSchema.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn file_tools_all_advertised() {
    use rmcp::ServiceExt;

    let (server_t, client_t) = tokio::io::duplex(8192);
    let bridge = Bridge::new(caps(), Box::new(mock::MockDevice));
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
    let names: std::collections::BTreeSet<&str> =
        tools.tools.iter().map(|t| t.name.as_ref()).collect();

    // The advertised set is exactly the eight file tools plus win32_echo.
    let mut expected: std::collections::BTreeSet<&str> = FILE_TOOLS.iter().copied().collect();
    expected.insert("win32_echo");
    assert_eq!(names, expected, "advertised tool set is the eight + echo");

    // Every file tool carries a JSON Schema 2020-12 inputSchema that is a
    // closed object (additionalProperties:false) with its listed args
    // required.
    let expect_args: std::collections::BTreeMap<&str, &[&str]> = [
        ("win32_read_file", &["path"][..]),
        ("win32_write_file", &["path", "data"][..]),
        ("win32_list_dir", &["path"][..]),
        ("win32_delete_file", &["path"][..]),
        ("win32_copy_file", &["source", "dest"][..]),
        ("win32_move_file", &["source", "dest"][..]),
        ("win32_make_dir", &["path"][..]),
        ("win32_remove_dir", &["path"][..]),
    ]
    .into_iter()
    .collect();
    for t in &tools.tools {
        let Some(args) = expect_args.get(t.name.as_ref()) else {
            continue;
        };
        let schema = &t.input_schema;
        assert_eq!(
            schema.get("type").and_then(|v| v.as_str()),
            Some("object"),
            "{} inputSchema is an object",
            t.name
        );
        assert_eq!(
            schema.get("additionalProperties"),
            Some(&serde_json::Value::Bool(false)),
            "{} inputSchema is closed (additionalProperties:false)",
            t.name
        );
        let required: std::collections::BTreeSet<&str> = schema
            .get("required")
            .and_then(|v| v.as_array())
            .map(|a| a.iter().filter_map(|v| v.as_str()).collect())
            .unwrap_or_default();
        let want: std::collections::BTreeSet<&str> = args.iter().copied().collect();
        assert_eq!(
            required, want,
            "{} requires exactly its listed args",
            t.name
        );
        // Each listed arg is typed as a string.
        let props = schema
            .get("properties")
            .and_then(|v| v.as_object())
            .unwrap_or_else(|| panic!("{} has schema properties", t.name));
        for arg in *args {
            assert_eq!(
                props
                    .get(*arg)
                    .and_then(|p| p.get("type"))
                    .and_then(|v| v.as_str()),
                Some("string"),
                "{} arg {arg} is a string",
                t.name
            );
        }
    }

    let _ = client.cancel().await;
}

/// invariant.FileToolHintsAreHonest — per-tool destructiveHint/readOnlyHint
/// match the settled table: read_file/list_dir are read-only; write_file/
/// delete_file/move_file/remove_dir are destructive; copy_file/make_dir
/// carry neither (copy is fail-if-exists, so it destroys nothing); no tool
/// is both read-only and destructive.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn file_tool_hints_are_honest() {
    use rmcp::ServiceExt;

    let (server_t, client_t) = tokio::io::duplex(8192);
    let bridge = Bridge::new(caps(), Box::new(mock::MockDevice));
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

    let read_only = |name: &str| -> bool {
        tools
            .tools
            .iter()
            .find(|t| t.name == name)
            .and_then(|t| t.annotations.as_ref())
            .and_then(|a| a.read_only_hint)
            .unwrap_or(false)
    };
    let destructive = |name: &str| -> bool {
        tools
            .tools
            .iter()
            .find(|t| t.name == name)
            .and_then(|t| t.annotations.as_ref())
            .and_then(|a| a.destructive_hint)
            .unwrap_or(false)
    };

    for name in ["win32_read_file", "win32_list_dir"] {
        assert!(read_only(name), "{name} is readOnlyHint");
        assert!(!destructive(name), "{name} is not destructiveHint");
    }
    for name in [
        "win32_write_file",
        "win32_delete_file",
        "win32_move_file",
        "win32_remove_dir",
    ] {
        assert!(destructive(name), "{name} is destructiveHint");
        assert!(!read_only(name), "{name} is not readOnlyHint");
    }
    // copy_file/make_dir carry neither hint — copy is fail-if-exists so it
    // destroys nothing; make_dir only adds.
    for name in ["win32_copy_file", "win32_make_dir"] {
        assert!(!destructive(name), "{name} carries no destructiveHint");
        assert!(!read_only(name), "{name} carries no readOnlyHint");
    }
    // No tool is ever both read-only and destructive.
    for name in FILE_TOOLS {
        assert!(
            !(read_only(name) && destructive(name)),
            "{name} is not both read-only and destructive"
        );
    }

    let _ = client.cancel().await;
}

/// invariant.ShellFileBuiltinsNotExposed — the supersession pin: no
/// advertised tool is named after a shell file builtin
/// (dir/type/del/copy/ren/md/rd). File manipulation is reachable only
/// through the API-backed win32_* tools.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn shell_builtins_not_exposed() {
    use rmcp::ServiceExt;

    let (server_t, client_t) = tokio::io::duplex(8192);
    let bridge = Bridge::new(caps(), Box::new(mock::MockDevice));
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
    let builtins = ["dir", "type", "del", "copy", "ren", "md", "rd"];
    for t in &tools.tools {
        assert!(
            !builtins.contains(&t.name.as_ref()),
            "no tool is named after a shell builtin; found {}",
            t.name
        );
    }

    let _ = client.cancel().await;
}

/// invariant.FileToolsAreDirectRelays, entity-optional.Tool.device_cmd,
/// entity-fields.Tool (extended) — calling each of the eight file tools
/// sends exactly its device command with the name-mapped arguments,
/// observed at the recording mock device. The copy/move source->path +
/// dest->dest mapping is asserted explicitly.
#[tokio::test]
async fn file_tools_relay_to_device_cmds() {
    let dev = mock::RecordingDevice::ok("done");
    let log = dev.received.clone();
    let bridge = Bridge::new(caps(), Box::new(dev));

    // Single-path tools: tool -> device cmd, path -> path.
    let single = [
        ("read", "win32_read_file"),
        ("list", "win32_list_dir"),
        ("delete", "win32_delete_file"),
        ("mkdir", "win32_make_dir"),
        ("rmdir", "win32_remove_dir"),
    ];
    bridge
        .win32_read_file(Parameters(PathParams {
            path: "C:\\a.txt".to_string(),
        }))
        .await
        .unwrap();
    bridge
        .win32_list_dir(Parameters(PathParams {
            path: "C:\\dir".to_string(),
        }))
        .await
        .unwrap();
    bridge
        .win32_delete_file(Parameters(PathParams {
            path: "C:\\gone.txt".to_string(),
        }))
        .await
        .unwrap();
    bridge
        .win32_make_dir(Parameters(PathParams {
            path: "C:\\new".to_string(),
        }))
        .await
        .unwrap();
    bridge
        .win32_remove_dir(Parameters(PathParams {
            path: "C:\\old".to_string(),
        }))
        .await
        .unwrap();

    // write_file: path -> path, data -> data (base64 relayed verbatim).
    bridge
        .win32_write_file(Parameters(WriteParams {
            path: "C:\\w.bin".to_string(),
            data: "aGVsbG8=".to_string(),
        }))
        .await
        .unwrap();

    // copy_file / move_file: source -> path, dest -> dest.
    bridge
        .win32_copy_file(Parameters(SourceDestParams {
            source: "C:\\src.txt".to_string(),
            dest: "C:\\dst.txt".to_string(),
        }))
        .await
        .unwrap();
    bridge
        .win32_move_file(Parameters(SourceDestParams {
            source: "C:\\from.txt".to_string(),
            dest: "C:\\to.txt".to_string(),
        }))
        .await
        .unwrap();

    let received = log.lock().unwrap();
    // Eight calls, one per tool, in the order issued above.
    assert_eq!(received.len(), 8, "one device call per file tool");

    let by_cmd = |cmd: &str| -> &serde_json::Map<String, serde_json::Value> {
        &received
            .iter()
            .find(|r| r.cmd == cmd)
            .unwrap_or_else(|| panic!("device received a {cmd} command"))
            .args
    };
    let arg = |cmd: &str, key: &str| -> String {
        by_cmd(cmd)
            .get(key)
            .and_then(|v| v.as_str())
            .unwrap_or_else(|| panic!("{cmd} carries a string {key}"))
            .to_string()
    };

    // Single-path mapping: the tool's device cmd is sent, path -> path.
    let _ = single; // documents the (cmd, tool) pairing exercised above.
    assert_eq!(arg("read", "path"), "C:\\a.txt");
    assert_eq!(arg("list", "path"), "C:\\dir");
    assert_eq!(arg("delete", "path"), "C:\\gone.txt");
    assert_eq!(arg("mkdir", "path"), "C:\\new");
    assert_eq!(arg("rmdir", "path"), "C:\\old");

    // write: path -> path, data -> data, base64 relayed verbatim.
    assert_eq!(arg("write", "path"), "C:\\w.bin");
    assert_eq!(arg("write", "data"), "aGVsbG8=");

    // copy: source -> path, dest -> dest (asserted explicitly).
    assert_eq!(arg("copy", "path"), "C:\\src.txt");
    assert_eq!(arg("copy", "dest"), "C:\\dst.txt");
    assert!(
        !by_cmd("copy").contains_key("source"),
        "copy maps source onto the wire `path`, never a `source` field"
    );

    // move: source -> path, dest -> dest (asserted explicitly).
    assert_eq!(arg("move", "path"), "C:\\from.txt");
    assert_eq!(arg("move", "dest"), "C:\\to.txt");
    assert!(
        !by_cmd("move").contains_key("source"),
        "move maps source onto the wire `path`, never a `source` field"
    );
}

/// Cross-module scenario (device reply -> MCP result, the 5.0 generic
/// mapping): a device status:"error" reply to a copy surfaces as an
/// isError tool result carrying the reason; a device ok reply surfaces as
/// a success result with structuredContent.
#[tokio::test]
async fn file_tool_device_error_is_iserror() {
    // Device refuses the copy with the fail-if-exists reason.
    let bridge = Bridge::new(
        caps(),
        Box::new(mock::RecordingDevice::error("file exists")),
    );
    let r = bridge
        .win32_copy_file(Parameters(SourceDestParams {
            source: "C:\\a".to_string(),
            dest: "C:\\b".to_string(),
        }))
        .await
        .unwrap();
    assert_eq!(r.is_error, Some(true), "device error -> isError");
    let text: String = r
        .content
        .iter()
        .filter_map(|c| c.as_text())
        .map(|t| t.text.as_str())
        .collect();
    assert!(
        text.contains("file exists"),
        "the device's error reason surfaces in the result text; got {text:?}"
    );

    // Device acks the copy: success with a structuredContent mirror.
    let bridge = Bridge::new(caps(), Box::new(mock::RecordingDevice::ok("copied")));
    let r = bridge
        .win32_copy_file(Parameters(SourceDestParams {
            source: "C:\\a".to_string(),
            dest: "C:\\b".to_string(),
        }))
        .await
        .unwrap();
    assert_eq!(r.is_error, Some(false), "device ok -> success");
    let sc = r.structured_content.expect("ok carries structuredContent");
    assert_eq!(
        sc.get("message").and_then(|v| v.as_str()),
        Some("copied"),
        "the ok reply's fields are mirrored into structuredContent"
    );
}
