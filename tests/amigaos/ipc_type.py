#!/usr/bin/env python3
"""Type one line + Return into the currently focused Amiga window over the
Amiberry IPC socket (no new Execute Command window)."""
import asyncio
import os
import sys
import time

MCP_SRC = os.environ.get("AMIBERRY_MCP_SRC")
if not MCP_SRC:
    sys.exit("AMIBERRY_MCP_SRC must point at the amiberry-mcp-server 'src' directory")
sys.path.insert(0, MCP_SRC)
from amiberry_mcp.ipc_client import AmiberryIPCClient  # noqa: E402

SOCK = os.environ.get("AMIBERRY_IPC_SOCK") or os.path.join(
    os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()), "amiberry.sock")


async def main():
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=SOCK)
    await c.ping()
    typed = await c.send_text(sys.argv[1] + "\n", delay=0.05)
    print("typed:", typed)
    await asyncio.sleep(1.5)
    await c.screenshot("type_%d.png" % time.time_ns())
    await c.close()


asyncio.run(main())
