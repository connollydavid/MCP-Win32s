//! The device client: connects to the MCP-Win32s device over TCP or
//! serial, frames newline-JSON, reads the ready handshake, and
//! round-trips commands. Behind the `Device` trait so tests inject a
//! mock. Single-client-sequential, matching the device.

use crate::capabilities::Capabilities;
use crate::wire::{Command, Ready, Response};
use anyhow::{anyhow, bail, Context, Result};
use async_trait::async_trait;
use tokio::io::{
    split, AsyncBufReadExt, AsyncRead, AsyncWrite, AsyncWriteExt, BufReader, Lines, ReadHalf,
    WriteHalf,
};

/// A connection to the device: send a command, await its correlated
/// response. The bridge holds one and serialises calls to it.
#[async_trait]
pub trait Device: Send {
    async fn call(&mut self, cmd: &Command) -> Result<Response>;
}

/// A device over any byte stream (TCP, serial), framed as newline-JSON.
pub struct StreamDevice<S> {
    lines: Lines<BufReader<ReadHalf<S>>>,
    write: WriteHalf<S>,
}

impl<S> StreamDevice<S>
where
    S: AsyncRead + AsyncWrite + Unpin + Send,
{
    pub fn new(stream: S) -> Self {
        let (r, w) = split(stream);
        StreamDevice {
            lines: BufReader::new(r).lines(),
            write: w,
        }
    }

    /// Read the first line and parse it as the ready handshake.
    pub async fn read_ready(&mut self) -> Result<Ready> {
        let line = self
            .lines
            .next_line()
            .await
            .context("reading device ready line")?
            .ok_or_else(|| anyhow!("device closed before sending ready"))?;
        let ready: Ready = serde_json::from_str(&line)
            .with_context(|| format!("parsing device ready message: {line}"))?;
        if !ready.is_ready() {
            bail!("first device line was not a ready message: {line}");
        }
        Ok(ready)
    }

    async fn write_line(&mut self, s: &str) -> Result<()> {
        self.write.write_all(s.as_bytes()).await?;
        self.write.write_all(b"\n").await?;
        self.write.flush().await?;
        Ok(())
    }
}

#[async_trait]
impl<S> Device for StreamDevice<S>
where
    S: AsyncRead + AsyncWrite + Unpin + Send,
{
    async fn call(&mut self, cmd: &Command) -> Result<Response> {
        let payload = serde_json::to_string(cmd).context("serialising command")?;
        self.write_line(&payload).await?;
        // Single-client-sequential: the next line is the response. Skip
        // any blank lines defensively; a closed link is an error.
        loop {
            let line = self
                .lines
                .next_line()
                .await
                .context("reading device response")?
                .ok_or_else(|| anyhow!("device closed during a request"))?;
            if line.trim().is_empty() {
                continue;
            }
            let resp: Response = serde_json::from_str(&line)
                .with_context(|| format!("parsing device response: {line}"))?;
            return Ok(resp);
        }
    }
}

/// Connect to the device per the operator's argument and complete the
/// ready handshake. Returns the negotiated capabilities and the live
/// device client.
///
/// Argument forms: `--tcp HOST:PORT` | `--serial PATH[:BAUD]`.
pub async fn connect(
    mut args: impl Iterator<Item = String>,
) -> Result<(Capabilities, Box<dyn Device>)> {
    let kind = args.next().unwrap_or_else(|| "--tcp".to_string());
    let target = args.next().ok_or_else(|| {
        anyhow!("usage: mcp-w32s-bridge (--tcp HOST:PORT | --serial PATH[:BAUD])")
    })?;

    match kind.as_str() {
        "--tcp" => {
            let stream = tokio::net::TcpStream::connect(&target)
                .await
                .with_context(|| format!("connecting TCP {target}"))?;
            let mut dev = StreamDevice::new(stream);
            let ready = dev.read_ready().await?;
            let caps =
                Capabilities::from_ready(ready.codepage, ready.version.clone(), &ready.features);
            Ok((caps, Box::new(dev)))
        }
        "--serial" => {
            let (path, baud) = match target.rsplit_once(':') {
                Some((p, b)) if b.chars().all(|c| c.is_ascii_digit()) => {
                    (p.to_string(), b.parse::<u32>().unwrap_or(115_200))
                }
                _ => (target.clone(), 115_200),
            };
            let stream = tokio_serial::SerialStream::open(&tokio_serial::new(&path, baud))
                .with_context(|| format!("opening serial {path} @ {baud}"))?;
            let mut dev = StreamDevice::new(stream);
            let ready = dev.read_ready().await?;
            let caps =
                Capabilities::from_ready(ready.codepage, ready.version.clone(), &ready.features);
            Ok((caps, Box::new(dev)))
        }
        other => bail!("unknown transport flag {other} (expected --tcp or --serial)"),
    }
}
