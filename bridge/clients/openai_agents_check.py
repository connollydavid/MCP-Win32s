"""
Offline conformance check for the MCP-Win32s bridge using the OpenAI Agents SDK.

Requirements:
  pip install openai-agents

No OpenAI API key is needed. This script exercises only the MCP plumbing:
MCPServerStdio connects to the bridge over stdio, the bridge connects to the
device over TCP. No LLM call is made.

The device must be reachable at the address given on the command line (or the
default 127.0.0.1:31800). In CI this script runs against the conformance
harness's Wine-hosted device; locally it runs against a native or Wine device.

Usage:
  python3 openai_agents_check.py [HOST:PORT]

Exit codes:
  0  all assertions passed
  1  a conformance assertion failed
  2  import or connection error
"""

import asyncio
import sys
import os

BRIDGE_BINARY = os.path.join(
    os.path.dirname(__file__),
    "..", "..", "bridge", "target", "release", "mcp-w32s-bridge",
)
BRIDGE_BINARY = os.path.normpath(BRIDGE_BINARY)

DEVICE_ADDR = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:31800"

REQUIRED_TOOLS = {"win32_exec", "win32_list_commands"}


async def run() -> None:
    try:
        from agents.mcp import MCPServerStdio  # type: ignore[import]
    except ImportError:
        print(
            "ERROR: openai-agents not installed. Run: pip install openai-agents",
            file=sys.stderr,
        )
        sys.exit(2)

    server = MCPServerStdio(
        name="win32s",
        params={
            "command": BRIDGE_BINARY,
            "args": ["--tcp", DEVICE_ADDR],
        },
    )

    async with server:
        # --- check 1: tools/list must include the required tool names ---
        tools = await server.list_tools()
        tool_names = {t.name for t in tools}
        missing = REQUIRED_TOOLS - tool_names
        if missing:
            print(
                f"FAIL tools/list: required tools absent: {sorted(missing)}",
                file=sys.stderr,
            )
            print(f"  present: {sorted(tool_names)}", file=sys.stderr)
            sys.exit(1)
        print(f"OK tools/list: {sorted(REQUIRED_TOOLS)} present (of {len(tool_names)} total)")

        # --- check 2: tools/call win32_list_commands must return a result ---
        # The SDK call form varies slightly between versions; try both the
        # positional-dict form and the keyword form.
        try:
            result = await server.call_tool("win32_list_commands", {})
        except TypeError:
            # older SDK versions use a single positional arguments dict
            result = await server.call_tool("win32_list_commands", arguments={})  # type: ignore[call-arg]

        if result is None:
            print("FAIL tools/call: win32_list_commands returned None", file=sys.stderr)
            sys.exit(1)

        # The result object may be a CallToolResult-like with an is_error flag,
        # or a raw dict, depending on the SDK version. Check both forms.
        is_err = getattr(result, "isError", None)
        if is_err is None and isinstance(result, dict):
            is_err = result.get("isError", False)
        if is_err:
            print(
                f"FAIL tools/call: win32_list_commands returned isError=True: {result}",
                file=sys.stderr,
            )
            sys.exit(1)

        print("OK tools/call: win32_list_commands returned a non-error result")

    print("conformance: all checks passed")


if __name__ == "__main__":
    asyncio.run(run())
