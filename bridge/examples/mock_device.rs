//! A mock MCP-Win32s device for conformance/integration runs: listens on
//! TCP, sends the ready message, then answers `echo` (and replies a
//! recoverable error to any other command). Models the device side of
//! specs/wire-contract.allium. Usage: `mock_device [HOST:PORT]`.

use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::TcpListener;

#[tokio::main(flavor = "current_thread")]
async fn main() -> std::io::Result<()> {
    let addr = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "127.0.0.1:31900".to_string());
    let listener = TcpListener::bind(&addr).await?;
    eprintln!("mock device listening on {addr}");
    loop {
        let (sock, _) = listener.accept().await?;
        tokio::spawn(handle(sock));
    }
}

async fn handle(sock: tokio::net::TcpStream) -> std::io::Result<()> {
    let (r, mut w) = sock.into_split();
    let ready = r#"{"status":"ready","codepage":437,"version":"MockWindows 1.0 (NT)","transport":"tcp","features":{"is_win32s":false,"is_win9x":false,"is_nt":true,"is_wow64":false,"threads":true,"job_objects":true,"ctrl_events":true,"pty":false,"binary_classify":"GetBinaryTypeA","process_mitigation":false}}"#;
    w.write_all(ready.as_bytes()).await?;
    w.write_all(b"\n").await?;
    w.flush().await?;
    let mut lines = BufReader::new(r).lines();
    while let Some(line) = lines.next_line().await? {
        if line.trim().is_empty() {
            continue;
        }
        let v: serde_json::Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let id = v.get("id").and_then(|x| x.as_str()).unwrap_or("");
        let cmd = v.get("cmd").and_then(|x| x.as_str()).unwrap_or("");
        let resp = if cmd == "echo" {
            let data = v.get("line").and_then(|x| x.as_str()).unwrap_or("");
            serde_json::json!({"id": id, "status": "ok", "data": data})
        } else {
            serde_json::json!({"id": id, "status": "error", "error": "unknown command"})
        };
        w.write_all(resp.to_string().as_bytes()).await?;
        w.write_all(b"\n").await?;
        w.flush().await?;
    }
    Ok(())
}
