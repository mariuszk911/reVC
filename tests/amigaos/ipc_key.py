#!/usr/bin/env python3
"""Send named key presses (and optional combo) to the running Amiberry."""
import asyncio
import os
import sys

MCP_SRC = os.environ.get("AMIBERRY_MCP_SRC")
if not MCP_SRC:
    sys.exit("AMIBERRY_MCP_SRC must point at the amiberry-mcp-server 'src' directory")
sys.path.insert(0, MCP_SRC)
from amiberry_mcp.ipc_client import AmiberryIPCClient, resolve_key_name  # noqa: E402

SOCK = os.environ.get("AMIBERRY_IPC_SOCK") or os.path.join(
    os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()), "amiberry.sock")


async def main():
    keys = sys.argv[1:] or ["return"]
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=SOCK)
    print("ping:", await c.ping())
    for k in keys:
        code = resolve_key_name(k)
        await c.send_key(code, True)
        await asyncio.sleep(0.06)
        await c.send_key(code, False)
        print("key:", k, code)
        await asyncio.sleep(0.2)
    await c.close()


asyncio.run(main())
