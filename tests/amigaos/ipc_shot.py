#!/usr/bin/env python3
"""Take one IPC screenshot (written to the Amiberry CWD) and exit."""
import asyncio
import os
import sys

MCP_SRC = os.environ.get("AMIBERRY_MCP_SRC")
if not MCP_SRC:
    sys.exit("AMIBERRY_MCP_SRC must point at the amiberry-mcp-server 'src' directory")
sys.path.insert(0, MCP_SRC)
from amiberry_mcp.ipc_client import AmiberryIPCClient  # noqa: E402

SOCK = os.environ.get("AMIBERRY_IPC_SOCK") or os.path.join(
    os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()), "amiberry.sock")


async def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "ipc_shot.png"
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=SOCK)
    print("ping:", await c.ping())
    print("shot:", await c.screenshot(name))
    await c.close()


asyncio.run(main())
