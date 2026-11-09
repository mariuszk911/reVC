#!/usr/bin/env python3
"""Run one or more AmigaDOS commands through a fresh Right-Amiga+E
'Execute Command' window each time, over the Amiberry IPC socket."""
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


async def tap(c, name):
    await c.send_key(resolve_key_name(name), True)
    await asyncio.sleep(0.06)
    await c.send_key(resolve_key_name(name), False)
    await asyncio.sleep(0.06)


async def combo(c, mod, key):
    await c.send_key(resolve_key_name(mod), True)
    await asyncio.sleep(0.06)
    await tap(c, key)
    await c.send_key(resolve_key_name(mod), False)


async def main():
    cmds = sys.argv[1:]
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=SOCK)
    print("ping:", await c.ping())
    for i, cmd in enumerate(cmds):
        await combo(c, "right_amiga", "e")
        await asyncio.sleep(1.5)
        await c.screenshot("floatmath_%02d.png" % i)
        typed = await c.send_text(cmd + "\n", delay=0.04)
        print("cmd %r typed=%s" % (cmd, typed))
        await asyncio.sleep(9.0)
        await c.screenshot("floatmath_%02d_done.png" % i)
    await c.close()


asyncio.run(main())
