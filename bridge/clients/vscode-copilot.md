# VS Code Copilot — MCP-Win32s manual demo

This is a manual, user-run acceptance step. It verifies that VS Code Copilot
in Agent mode can discover and call the Win32s tools via the bridge.

## Prerequisites

- VS Code with the GitHub Copilot extension (Agent mode requires Copilot Chat).
- The bridge binary built: `cargo build --release` inside `bridge/`.
- The device running and reachable (natively on Windows, or Wine on Linux):
  `mcp-w32s.exe /TCP:31800` (adjust the port to match `mcp.json`).

## Steps

1. Copy `bridge/clients/vscode/mcp.json` into your workspace's `.vscode/` folder.

2. Start the device:
   - **Windows**: `bridge\target\release\mcp-w32s-bridge` reads from the next
     step; start the device first: `mcp-w32s.exe /TCP:31800`
   - **Linux/Wine**: `wine build/mingw/mcp-w32s.exe /TCP:31800 &`

3. Open the Command Palette (`Ctrl+Shift+P` / `Cmd+Shift+P`) and run
   **Developer: Reload Window** so VS Code picks up the new `mcp.json`.

4. Open Copilot Chat and switch to **Agent mode** using the mode selector at
   the bottom of the chat panel. MCP tools are only visible in Agent mode —
   they do not appear in Ask or Edit mode.

5. Confirm the `win32_*` tools are listed. You can check by clicking the
   tools icon in the Copilot Chat input area; the `win32s` server should appear
   with its tools (at minimum `win32_exec` and `win32_list_commands`).

6. Run a representative call. For example, ask Copilot:

   > List the available Win32 commands on the connected device.

   Copilot should invoke `win32_list_commands` and return the device's
   command catalog as structured JSON.

## What this verifies

- The bridge binary starts correctly when VS Code spawns it.
- The bridge connects to the device over TCP and completes the ready handshake.
- VS Code Copilot in Agent mode discovers and exposes the `win32_*` tools.
- A `win32_list_commands` call round-trips successfully and returns a result.

## Troubleshooting

- **Tools not visible**: ensure Copilot Chat is in **Agent** mode (not Ask or Edit).
- **Bridge fails to start**: check that `bridge/target/release/mcp-w32s-bridge`
  exists and is executable; rebuild with `cargo build --release`.
- **Device unreachable**: confirm the device is running and listening on the port
  in `mcp.json` (default `127.0.0.1:31800`). Adjust the `args` array if the port differs.
- **`${workspaceFolder}` not resolved**: VS Code expands this variable when the
  folder containing `mcp.json` is the open workspace root. If it is not, replace it
  with the absolute path to the bridge binary.
