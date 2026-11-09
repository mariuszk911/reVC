#!/usr/bin/env python3
"""Open one Right-Amiga+E requester and run reVC.db from the game drawer
(DH12:). The OS 3.2 Shell supports `&&`, so one line can CD and run."""
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
GAME_DIR = "DH12:"
CMDS = "CD %s && reVC.db\n" % GAME_DIR


async def tap(c, name):
    await c.send_key(resolve_key_name(name), True)
    await asyncio.sleep(0.06)
    await c.send_key(resolve_key_name(name), False)
    await asyncio.sleep(0.06)


async def main():
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=SOCK)
    print("ping:", await c.ping())
    await c.send_key(resolve_key_name("right_amiga"), True)
    await asyncio.sleep(0.06)
    await tap(c, "e")
    await c.send_key(resolve_key_name("right_amiga"), False)
    await asyncio.sleep(1.5)
    typed = await c.send_text(CMDS, delay=0.05)
    print("typed:", typed)
    await asyncio.sleep(6.0)
    await c.screenshot("exec_revc.png")
    await c.close()


asyncio.run(main())
