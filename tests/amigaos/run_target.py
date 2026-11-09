#!/usr/bin/env python3
"""Run the cross-compiled m68k unit tests inside Amiberry and report results.

Pipeline:
  1. resolve the expected tests + per-test logs (from the generated manifest)
  2. attach to a running Amiberry over IPC, or launch it (AMIBERRY_LAUNCH_CMD)
  3. hard-reset (refreshes the DH11 host-dir cache), wait for boot
  4. open the Workbench "Execute a file" requester (Right-Amiga+E) and type
     `CD <amiga-dir> && Execute run_tests` (the OS 3.2 Shell supports `&&`)
  5. wait for build/amigaos/test/target_done.txt (primary), TARGET:ALLDONE over
     serial (--serial), or all expected logs going terminal + quiet
  6. classify every expected log and exit non-zero on FAIL/MISSING

`--no-ipc --logs-dir <dir>` only parses already-collected logs and needs no
MCP venv / emulator (real-HW path).
"""
import argparse
import asyncio
import glob
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
# Do not drop __pycache__ into tests/amigaos/ when imported directly.
sys.dont_write_bytecode = True

import parse_target_logs as ptl  # noqa: E402
import gen_target_script as gts  # noqa: E402

DEFAULT_LOGS_DIR = os.path.join(ROOT, "build", "amigaos/test")
DEFAULT_MCP_SRC = os.environ.get("AMIBERRY_MCP_SRC", "")
DEFAULT_SOCK = os.environ.get("AMIBERRY_IPC_SOCK") or os.path.join(
    os.environ.get("XDG_RUNTIME_DIR", "/run/user/%d" % os.getuid()),
    "amiberry.sock")
DEFAULT_SERIAL_PORT = 12345
# Test drawer on the Amiga side. The generated run_tests is drive-agnostic, so
# the driver CD's here before `Execute run_tests`.
DEFAULT_AMIGA_DIR = "DH11:"
DEFAULT_CONFIG = os.environ.get(
    "AMIBERRY_CONFIG",
    os.path.expanduser("~/Amiberry/Configurations/B1260_serialtest.uae"))
DEFAULT_LAUNCH_CMD = os.environ.get(
    "AMIBERRY_LAUNCH_CMD",
    "flatpak-spawn --host env DISPLAY=${{DISPLAY:-:0}} /usr/bin/amiberry "
    "-f {config} -G")


class SerialWatcher(threading.Thread):
    """Best-effort serial reader; sets `found` on TARGET:ALLDONE."""

    def __init__(self, host, port):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.found = threading.Event()
        self.lines = []
        self.error = None
        self.stop = False

    def run(self):
        try:
            s = socket.create_connection((self.host, self.port), timeout=2)
            s.settimeout(1.0)
        except OSError as exc:
            # Not fatal: the done-file is the primary completion signal, but
            # --serial was explicitly requested and is now inert.
            self.error = str(exc)
            print("run_target: WARNING: cannot read serial %s:%s (%s); "
                  "done-file is the only completion signal" % (
                      self.host, self.port, exc), file=sys.stderr)
            return
        buf = b""
        try:
            while not self.stop:
                try:
                    data = s.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode("latin1").strip()
                    if text:
                        self.lines.append(text)
                        if "TARGET:ALLDONE" in text:
                            self.found.set()
        finally:
            try:
                s.close()
            except OSError:
                pass


_DEVICE_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]*:")


def host_log_path(logs_dir, amiga_log):
    """Map a manifest `DH11:foo.log` (any device prefix) to a host path."""
    name = _DEVICE_RE.sub("", amiga_log, count=1).lstrip("/")
    return os.path.join(logs_dir, name)


def load_entries(args):
    """Return [(name, host_log_path_or_None)] for the expected tests."""
    manifest_path = args.manifest or os.path.join(args.logs_dir,
                                                   "target_manifest.json")
    manifest = None
    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path) as fh:
                manifest = json.load(fh)
        except (OSError, ValueError):
            manifest = None

    if args.tests:
        names = args.tests
    elif manifest and manifest.get("tests"):
        names = [e["name"] for e in manifest["tests"]]
    else:
        # No manifest: classify whatever logs are present.
        logs = sorted(glob.glob(os.path.join(args.logs_dir, "*.log")))
        return [(os.path.splitext(os.path.basename(p))[0], p) for p in logs]

    by_name = {}
    if manifest:
        for e in manifest.get("tests", []):
            by_name[e["name"]] = e
    entries = []
    for name in names:
        e = by_name.get(name)
        if e and e.get("log"):
            logfile = host_log_path(args.logs_dir, e["log"])
        else:
            logfile = os.path.join(args.logs_dir, gts.log_name_for(HERE, name))
        entries.append((name, logfile))
    return entries


def clean_stale(args, entries):
    for _, logfile in entries:
        try:
            os.remove(logfile)
        except OSError:
            pass
    for stale in ("target_done.txt", "target_serial.log"):
        try:
            os.remove(os.path.join(args.logs_dir, stale))
        except OSError:
            pass


def report(entries):
    """Classify all expected logs; print a table. Returns exit code."""
    print("== target results ==")
    rc = 0
    for name, logfile in entries:
        if logfile and os.path.isfile(logfile):
            res = ptl.classify_file(logfile, name)
        else:
            res = ptl.Result(name, ptl.MISSING,
                             detail="log not found: %s" % (logfile or name))
        if res.status in (ptl.FAIL, ptl.MISSING):
            rc = 1
        line = "%-5s %-20s" % (res.status, name)
        if res.detail and res.status != ptl.PASS:
            line += "  (%s)" % res.detail
        print(line)
    return rc


def last_sentinel(logs_dir):
    path = os.path.join(logs_dir, "target_serial.log")
    last = None
    try:
        with open(path, "r", errors="replace") as fh:
            for ln in fh:
                if ln.startswith("TARGET:"):
                    last = ln.strip()
    except OSError:
        pass
    return last


def all_settled(entries, settle):
    now = time.time()
    newest = 0.0
    for name, logfile in entries:
        if not logfile or not os.path.isfile(logfile):
            return False
        res = ptl.classify_file(logfile, name)
        if res.status == ptl.MISSING:
            return False
        try:
            newest = max(newest, os.path.getmtime(logfile))
        except OSError:
            # File vanished between isfile() and stat(); not settled yet.
            return False
    return (now - newest) >= settle


def parse_only(args):
    entries = load_entries(args)
    if not entries:
        print("run_target: no tests/logs found in %s" % args.logs_dir,
              file=sys.stderr)
        return 2
    return report(entries)


async def run_ipc(args):
    sys.path.insert(0, args.mcp_src)
    try:
        from amiberry_mcp.ipc_client import (AmiberryIPCClient,
                                             resolve_key_name)
    except ImportError as exc:
        print("run_target: cannot import amiberry_mcp from %s (%s)\n"
              "  set AMIBERRY_MCP_SRC to the amiberry-mcp-server/src dir"
              % (args.mcp_src, exc), file=sys.stderr)
        return 2

    sock = args.sock
    c = AmiberryIPCClient(prefer_dbus=False, socket_path=sock)

    async def ping_ok():
        try:
            return await c.ping()
        except Exception:
            return False

    if not await ping_ok():
        print("run_target: no IPC at %s, launching emulator" % sock)
        try:
            cmd = args.launch_cmd.format(config=args.config, sock=sock)
        except (KeyError, IndexError, ValueError):
            # Literal braces in a custom command: run it verbatim.
            cmd = args.launch_cmd
        print("run_target: %s" % cmd)
        subprocess.Popen(cmd, shell=True)
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            await asyncio.sleep(2)
            if await ping_ok():
                break
        else:
            print("run_target: emulator did not come up (sock=%s)\n"
                  "  launch: %s" % (sock, cmd), file=sys.stderr)
            return 2

    entries = load_entries(args)
    if not entries:
        print("run_target: no expected tests (missing %s and no --tests)"
              % os.path.join(args.logs_dir, "target_manifest.json"),
              file=sys.stderr)
        return 2
    clean_stale(args, entries)

    watcher = None
    if args.serial:
        watcher = SerialWatcher("127.0.0.1", args.serial_port)
        watcher.start()

    if not args.no_reset:
        print("run_target: hard reset, waiting %ss for boot" % args.boot_wait)
        try:
            await c.reset(hard=True)
        except Exception as exc:
            print("run_target: reset failed: %s" % exc, file=sys.stderr)
        await asyncio.sleep(args.boot_wait)

    try:
        # Right-Amiga+E opens the Workbench "Execute a file" requester. The OS
        # 3.2 Shell accepts `&&`, so the one-line command can CD to the test
        # drawer and run the drive-agnostic (relative) run_tests from there.
        await c.send_key(resolve_key_name("right_amiga"), True)
        await asyncio.sleep(0.06)
        await c.send_key(resolve_key_name("e"), True)
        await asyncio.sleep(0.06)
        await c.send_key(resolve_key_name("e"), False)
        await asyncio.sleep(0.06)
        await c.send_key(resolve_key_name("right_amiga"), False)
        await asyncio.sleep(1.5)
        try:
            await c.screenshot("target_start.png")
        except Exception:
            pass
        typed = await c.send_text(
            "CD %s && Execute run_tests\n" % args.amiga_dir, delay=0.04)
        print("run_target: typed CD %s && Execute run_tests (%s)"
              % (args.amiga_dir, typed))
    except Exception as exc:
        print("run_target: failed to start script: %s" % exc, file=sys.stderr)
        return 2

    done_file = os.path.join(args.logs_dir, "target_done.txt")
    deadline = time.time() + args.timeout
    reason = None
    while time.time() < deadline:
        await asyncio.sleep(1)
        if os.path.isfile(done_file):
            reason = "done-file"
            break
        if watcher and watcher.found.is_set():
            reason = "serial TARGET:ALLDONE"
            break
        if all_settled(entries, args.settle):
            reason = "all logs terminal + settled"
            break

    if watcher:
        watcher.stop = True
    await asyncio.sleep(1.5)
    try:
        await c.screenshot("target_end.png")
    except Exception:
        pass

    if reason is None:
        last = last_sentinel(args.logs_dir)
        print("run_target: TIMEOUT after %ss%s" % (
            args.timeout, "; last sentinel: %s" % last if last else ""),
            file=sys.stderr)
    else:
        print("run_target: completed (%s)" % reason)

    try:
        await c.close()
    except Exception:
        pass

    return report(entries)


def default_config():
    return os.environ.get("AMIBERRY_CONFIG", DEFAULT_CONFIG)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tests", nargs="*", default=None,
                    help="subset of test names to require")
    ap.add_argument("--logs-dir", default=DEFAULT_LOGS_DIR,
                    help="host dir mounted as DH11: (default build/amigaos/test)")
    ap.add_argument("--manifest", default=None,
                    help="target_manifest.json path (default <logs-dir>/...)")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--boot-wait", type=int, default=25)
    ap.add_argument("--settle", type=float, default=4.0)
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--serial", action="store_true",
                    help="also watch the serial TCP port for TARGET:ALLDONE")
    ap.add_argument("--serial-port", type=int,
                    default=int(os.environ.get("AMIBERRY_SERIAL_PORT",
                                               DEFAULT_SERIAL_PORT)))
    ap.add_argument("--no-ipc", action="store_true",
                    help="only parse existing logs (real-HW path)")
    ap.add_argument("--sock", default=os.environ.get("AMIBERRY_IPC_SOCK",
                                                     DEFAULT_SOCK))
    ap.add_argument("--mcp-src", default=os.environ.get("AMIBERRY_MCP_SRC",
                                                        DEFAULT_MCP_SRC))
    ap.add_argument("--amiga-dir",
                    default=os.environ.get("AMIBERRY_AMIGA_DIR",
                                           DEFAULT_AMIGA_DIR),
                    help="test drawer the driver CD's to before "
                         "`Execute run_tests` (default DH11:)")
    ap.add_argument("--config", default=default_config())
    ap.add_argument("--launch-cmd",
                    default=os.environ.get("AMIBERRY_LAUNCH_CMD",
                                           DEFAULT_LAUNCH_CMD))
    args = ap.parse_args(argv)

    args.logs_dir = os.path.abspath(args.logs_dir)

    if args.no_ipc:
        return parse_only(args)
    return asyncio.run(run_ipc(args))


if __name__ == "__main__":
    sys.exit(main())
