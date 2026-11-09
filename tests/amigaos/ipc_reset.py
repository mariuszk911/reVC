#!/usr/bin/env python3
"""Reset the running Amiberry emulation and wait for the Amiga to boot."""
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
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=SOCK)
    print("ping:", await c.ping())
    print("reset:", await c.reset(hard=True))
    await c.close()


asyncio.run(main())
