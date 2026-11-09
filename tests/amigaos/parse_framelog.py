#!/usr/bin/env python3
"""Parse `MINI_FRAMELOG` lines from a reVC.log and summarise frame timings.

The PiStorm3D MINI backend emits one line every 30 frames when built with
REVC_FRAMELOG=1 (build/amigaos/build.py)::

    mini: frame 1230: 53.4 ms/frame; switch 0.2, clear 1.1, lock 0.3,
          draw 28.4, skin 5.1 ms; meshes=... skinmeshes=... fog=... clear=...

This tool turns a captured log into the numbers the S1 acceptance criterion
needs: median / 95th-percentile / max ms/frame and the mean phase breakdown.
It is host-only and emulator-free.

Usage:
    parse_framelog.py reVC.log                     # one capture
    parse_framelog.py sync.log async.log           # A/B, prints the delta
"""
import argparse
import re
import statistics
import sys

# Keep this in sync with vendor/librw/src/mini/mini.cpp (MINI_FRAMELOG).
# The im2d/im3d block (T0) is optional so logs from before it still parse.
LINE_RE = re.compile(
    r"mini:\s+frame\s+(\d+):\s+([0-9.]+)\s+ms/frame;\s*"
    r"switch\s+([0-9.]+),\s*clear\s+([0-9.]+),\s*lock\s+([0-9.]+),\s*"
    r"draw\s+([0-9.]+),\s*skin\s+([0-9.]+)\s+ms;"
    r"(?:\s*im2d\s+([0-9.]+)\s+ms/(\d+)\s+calls/(\d+)\s+verts,\s*"
    r"im3d\s+([0-9.]+)\s+ms/(\d+)\s+calls/(\d+)\s+verts;)?"
)

# S2 Phase 0 LoadAllRequestedModels instrumentation (Streaming.cpp). A "slow"
# line is one barrier over the threshold; "stats" is the one aggregate dump at
# Shutdown. Counters are integer milliseconds, so plain \d+ is exact.
LOADALL_SLOW_RE = re.compile(
    r"cdvd_stream:\s+loadall slow:\s+id=(-?\d+)\s+size=(\d+)\s+"
    r"ioMs=(\d+)\s+cpuMs=(\d+)\s+reqs=(\d+)")
LOADALL_STATS_RE = re.compile(
    r"cdvd_stream:\s+loadall stats:\s+calls=(\d+)\s+reqs=(\d+)\s+bytes=(\d+)\s+"
    r"ioMs=(\d+)\s+cpuMs=(\d+)\s+worstMs=(\d+)\s+worstId=(-?\d+)")

# TXD per-texture load profiler (vendor/librw/src/mini/miniraster.cpp). A "slow"
# line is one texture whose total load crossed the threshold; "stats" is the one
# aggregate dump at CStreaming::Shutdown. All times are integer milliseconds.
TEXLOAD_SLOW_RE = re.compile(
    r"mini:\s+texload slow:\s+name=(\S+)\s+wxh=(\d+)x(\d+)\s+comp=(\d+)\s+"
    r"depth=(\d+)\s+read=(\d+)\s+decode=(\d+)\s+fmt=(\d+)\s+create=(\d+)\s+"
    r"conv=(\d+)\s+upload=(\d+)")
TEXLOAD_STATS_RE = re.compile(
    r"mini:\s+texload stats:\s+n=(\d+)\s+readMs=(\d+)\s+decodeMs=(\d+)\s+"
    r"fmtMs=(\d+)\s+createMs=(\d+)\s+convMs=(\d+)\s+uploadMs=(\d+)\s+"
    r"downscaleMs=(\d+)\s+maxTexSize=(-?\d+)")

# Bucket order/names for the texload report (read/decode/fmt/conv are CPU,
# create/upload are the GL side). Keep in sync with miniraster.cpp.
TEX_BUCKETS = ["readMs", "decodeMs", "fmtMs", "createMs", "convMs", "uploadMs"]

# Frame profiler (src/core/FrameProfile.h). The boot log carries the 30-frame
# aggregate ("prof: frames N: ms/frame min .. max .. avg ..; <slots>; <sub>;
# <game>"); profile_frames.txt and profile_slow.txt carry one line per frame
# ("prof: frame N: X ms; <slots>; <sub>; <game>"). All values are integer tenths
# of a millisecond. The librw-render counters moved to the pull query
# (RwMiniStats) and are no longer part of these lines.
PROF_AGG_RE = re.compile(
    r"prof:\s+frames\s+(\d+):\s+ms/frame\s+min\s+([0-9.]+)\s+max\s+([0-9.]+)\s+"
    r"avg\s+([0-9.]+);(.*)")
PROF_FRAME_RE = re.compile(r"prof:\s+frame\s+(\d+):\s+([0-9.]+)\s+ms;(.*)")
PROF_SLOT_RE = re.compile(r"([a-zA-Z0-9]+)\s+([0-9]+\.[0-9])")
# Slot order used by the ranked attribution (outer phases first, then the
# in-scene/in-process subsets, then the FP_GAME_* split of FP_PROCESS).
PROF_OUTER_SLOTS = ["outside", "timer", "process", "audio", "renderlist",
                    "prerender", "clear", "scene", "debug", "effects", "mblur",
                    "2d", "menus", "fade", "present"]
PROF_SUB_SLOTS = ["lit", "skincpu"]
PROF_GAME_SLOTS = ["stream", "script", "pre", "pop", "misc", "world", "sim",
                   "cars"]


def _after_lines(path, after):
    with open(path, "r", errors="replace") as fh:
        lines = fh.readlines()
    if after:
        last = max((i for i, ln in enumerate(lines) if after in ln), default=None)
        if last is None:
            raise SystemExit("%s: marker %r not found (need the GAMEPLAY_BEGIN "
                             "build)" % (path, after))
        lines = lines[last + 1:]
    return lines


def parse_loadall(path, after=None):
    """Parse the S2 Phase 0 `loadall slow`/`loadall stats` lines."""
    slow = []
    stats = None
    for line in _after_lines(path, after):
        m = LOADALL_STATS_RE.search(line)
        if m:
            stats = dict(calls=int(m.group(1)), reqs=int(m.group(2)),
                         bytes=int(m.group(3)), ioMs=int(m.group(4)),
                         cpuMs=int(m.group(5)), worstMs=int(m.group(6)),
                         worstId=int(m.group(7)))
            continue
        m = LOADALL_SLOW_RE.search(line)
        if m:
            slow.append(dict(id=int(m.group(1)), size=int(m.group(2)),
                             ioMs=int(m.group(3)), cpuMs=int(m.group(4)),
                             reqs=int(m.group(5))))
    if stats is None and not slow:
        raise SystemExit("%s: no loadall lines found (run with "
                         "REVC_STREAM_LOADALLLOG=1)" % path)
    return {"stats": stats, "slow": slow}


def describe_loadall(label, d):
    s = d["stats"]
    slow = d["slow"]
    print("%s loadall" % label)
    if s:
        total = s["ioMs"] + s["cpuMs"]
        share = (100.0 * s["ioMs"] / total) if total else 0.0
        print("  aggregate : calls=%d reqs=%d bytes=%.1f MiB"
              % (s["calls"], s["reqs"], s["bytes"] / (1024.0 * 1024.0)))
        print("  split     : ioMs=%d cpuMs=%d (io %.0f%% of io+cpu) "
              "worstMs=%d worstId=%d"
              % (s["ioMs"], s["cpuMs"], share, s["worstMs"], s["worstId"]))
    else:
        print("  aggregate : (no `loadall stats` line; run ended before Shutdown)")
    print("  slow lines: %d above threshold" % len(slow))
    for e in sorted(slow, key=lambda e: e["ioMs"] + e["cpuMs"], reverse=True)[:10]:
        print("    id=%-6d size=%-6d ioMs=%-6d cpuMs=%-6d reqs=%d"
              % (e["id"], e["size"], e["ioMs"], e["cpuMs"], e["reqs"]))
    return s


def parse_texload(path, after=None):
    """Parse the TXD load profiler `texload slow`/`texload stats` lines."""
    slow = []
    stats = None
    for line in _after_lines(path, after):
        m = TEXLOAD_STATS_RE.search(line)
        if m:
            stats = dict(n=int(m.group(1)), readMs=int(m.group(2)),
                         decodeMs=int(m.group(3)), fmtMs=int(m.group(4)),
                         createMs=int(m.group(5)), convMs=int(m.group(6)),
                         uploadMs=int(m.group(7)), downscaleMs=int(m.group(8)),
                         maxTexSize=int(m.group(9)))
            continue
        m = TEXLOAD_SLOW_RE.search(line)
        if m:
            slow.append(dict(name=m.group(1), w=int(m.group(2)), h=int(m.group(3)),
                             comp=int(m.group(4)), depth=int(m.group(5)),
                             read=int(m.group(6)), decode=int(m.group(7)),
                             fmt=int(m.group(8)), create=int(m.group(9)),
                             conv=int(m.group(10)), upload=int(m.group(11))))
    if stats is None and not slow:
        raise SystemExit("%s: no texload lines found (the TXD load profiler "
                         "was removed in 2026-10-25; this option only parses "
                         "historical logs)" % path)
    return {"stats": stats, "slow": slow}


def describe_texload(label, d):
    s = d["stats"]
    slow = d["slow"]
    print("%s texload" % label)
    if s:
        total = sum(s[b] for b in TEX_BUCKETS)
        print("  aggregate : n=%d maxTexSize=%d downscaleMs=%d"
              % (s["n"], s["maxTexSize"], s["downscaleMs"]))
        print("  buckets   : total=%d ms" % total)
        for b in TEX_BUCKETS:
            share = (100.0 * s[b] / total) if total else 0.0
            print("    %-9s %7d ms  %5.1f%%" % (b, s[b], share))
        gpu = s["createMs"] + s["uploadMs"]
        cpu = s["readMs"] + s["decodeMs"] + s["fmtMs"] + s["convMs"]
        print("  split     : create+upload(GPU) %d ms, read+decode+fmt+conv(CPU) %d ms"
              % (gpu, cpu))
    else:
        print("  aggregate : (no `texload stats` line; run ended before Shutdown)")
    print("  slow lines: %d above threshold" % len(slow))
    for e in sorted(slow, key=lambda e: sum(e[b] for b in
                    ("read", "decode", "fmt", "create", "conv", "upload")),
                    reverse=True)[:10]:
        print("    %-24s %dx%d comp=%d read=%-5d decode=%-5d create=%-5d "
              "conv=%-5d upload=%-5d"
              % (e["name"], e["w"], e["h"], e["comp"], e["read"], e["decode"],
                 e["create"], e["conv"], e["upload"]))
    return s


def _prof_slots(tail):
    slots = {}
    for m in PROF_SLOT_RE.finditer(tail):
        slots[m.group(1)] = float(m.group(2))
    return slots


def parse_prof(path, after=None):
    """Parse `prof:` aggregate and per-frame lines.

    A capture may contain either: the boot log has the 30-frame aggregates, the
    exit dump (profile_frames.txt) and the slow trace (profile_slow.txt) have one
    line per frame. Both formats are parsed; the per-frame lines drive the
    statistics when present.
    """
    frames = []
    aggs = []
    for line in _after_lines(path, after):
        m = PROF_AGG_RE.search(line)
        if m:
            slots = _prof_slots(m.group(5))
            aggs.append(dict(seq=int(m.group(1)), fmin=float(m.group(2)),
                             fmax=float(m.group(3)), favg=float(m.group(4)),
                             slots=slots))
            continue
        m = PROF_FRAME_RE.search(line)
        if m:
            slots = _prof_slots(m.group(3))
            frames.append(dict(seq=int(m.group(1)), ms=float(m.group(2)),
                               slots=slots))
    if not frames and not aggs:
        raise SystemExit("%s: no `prof:` lines found (build with REVC_PROFILE=1)"
                         % path)
    return {"frames": frames, "aggs": aggs}


def _slot_mean(frames, name):
    vals = [f["slots"][name] for f in frames if name in f["slots"]]
    return statistics.fmean(vals) if vals else 0.0


def describe_prof(label, d, ranked=True):
    frames = d["frames"]
    aggs = d["aggs"]
    print("%s prof" % label)
    if frames:
        ms = [f["ms"] for f in frames]
        print("  frames    : n=%d  median %.1f  p95 %.1f  max %.1f ms"
              % (len(ms), statistics.median(ms), percentile(ms, 95), max(ms)))
        # per-slot means over the whole capture
        names = PROF_OUTER_SLOTS + PROF_SUB_SLOTS + PROF_GAME_SLOTS
        means = [(n, _slot_mean(frames, n)) for n in names]
        frame_mean = statistics.fmean(ms)
        print("  phase split (mean ms, share of frame):")
        for n, v in sorted(means, key=lambda kv: kv[1], reverse=True):
            if v <= 0.0:
                continue
            share = (100.0 * v / frame_mean) if frame_mean else 0.0
            print("    %-10s %8.1f  %5.1f%%" % (n, v, share))
        if ranked:
            top = [(n, v) for n, v in means if v > 0.0]
            top.sort(key=lambda kv: kv[1], reverse=True)
            if top:
                print("  dominant  : %s (%.1f ms, %.1f%%)"
                      % (top[0][0], top[0][1],
                         (100.0 * top[0][1] / frame_mean) if frame_mean else 0.0))
    else:
        print("  frames    : (no per-frame `prof: frame` lines; using aggregates)")
    if aggs:
        a = aggs[-1]
        print("  aggregate : %d window(s); last frame %d: min %.1f max %.1f "
              "avg %.1f ms" % (len(aggs), a["seq"], a["fmin"], a["fmax"], a["favg"]))
    return statistics.median([f["ms"] for f in frames]) if frames else None


def parse(path, after=None):
    frames, switch, clear, lock, draw, skin = ([] for _ in range(6))
    im2d, im3d = [], []
    lines = _after_lines(path, after)
    for line in lines:
        m = LINE_RE.search(line)
        if not m:
            continue
        frames.append(float(m.group(2)))
        switch.append(float(m.group(3)))
        clear.append(float(m.group(4)))
        lock.append(float(m.group(5)))
        draw.append(float(m.group(6)))
        skin.append(float(m.group(7)))
        if m.group(8) is not None:
            im2d.append(float(m.group(8)))
            im3d.append(float(m.group(11)))
    if not frames:
        raise SystemExit("%s: no MINI_FRAMELOG lines found "
                         "(build with REVC_FRAMELOG=1)" % path)
    return {
        "n": len(frames),
        "frame": frames,
        "switch": statistics.fmean(switch),
        "clear": statistics.fmean(clear),
        "lock": statistics.fmean(lock),
        "draw": statistics.fmean(draw),
        "skin": statistics.fmean(skin),
        "im2d": statistics.fmean(im2d) if im2d else None,
        "im3d": statistics.fmean(im3d) if im3d else None,
    }


def percentile(values, p):
    """Nearest-rank percentile (no interpolation), p in [0, 100]."""
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, int(round((p / 100.0) * len(ordered) + 0.5)) - 1))
    return ordered[idx]


def over_95th(values, reference_p95):
    """Count samples above the reference capture's p95 (S1 acceptance)."""
    return sum(1 for v in values if v > reference_p95)


def describe(label, d):
    f = d["frame"]
    print("%s (n=%d windows)" % (label, d["n"]))
    print("  ms/frame : mean %6.1f  median %6.1f  p95 %6.1f  max %6.1f"
          % (statistics.fmean(f), statistics.median(f),
             percentile(f, 95), max(f)))
    print("  phases   : switch %5.1f  clear %5.1f  lock %5.1f  draw %6.1f  skin %5.1f"
          % (d["switch"], d["clear"], d["lock"], d["draw"], d["skin"]))
    if d.get("im2d") is not None:
        print("  im       : im2d %5.1f  im3d %5.1f" % (d["im2d"], d["im3d"]))
    return percentile(f, 95)


def _self_test():
    """Guard the `prof:` parser's slot lists against internal drift.

    The lists restate the FrameProfile.h enum/name order, so a rename or an added
    slot there can silently drop a metric from this tool's attribution. This
    builds a synthetic line in the exact emitted format and asserts every listed
    name is parsed back.
    """
    names = PROF_OUTER_SLOTS + PROF_SUB_SLOTS + PROF_GAME_SLOTS
    if len(names) != len(set(names)):
        raise SystemExit("self-test: profiler slot lists contain duplicates")

    line = ("prof: frame 1: 10.0 ms; %s ; %s ; %s"
            % (" ".join("%s 1.0" % n for n in PROF_OUTER_SLOTS),
               " ".join("%s 1.0" % n for n in PROF_SUB_SLOTS),
               " ".join("%s 1.0" % n for n in PROF_GAME_SLOTS)))
    m = PROF_FRAME_RE.search(line)
    if not m:
        raise SystemExit("self-test: per-frame regex did not match the emitted format")
    got = _prof_slots(m.group(3))
    missing = [n for n in names if n not in got]
    if missing:
        raise SystemExit("self-test: parser dropped slots: %s" % ", ".join(missing))
    print("parse_framelog --self-test: ok (%d slots)" % len(names))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", help="one log (summary) or two (A/B)")
    ap.add_argument("--self-test", action="store_true", dest="self_test",
                    help="validate the `prof:` parser's slot lists and "
                         "regexes, then exit")
    ap.add_argument("--after", default=None, metavar="TEXT",
                    help="only count windows after the last line containing TEXT "
                         "(e.g. GAMEPLAY_BEGIN); makes a same-save A/B comparable")
    ap.add_argument("--loadall", action="store_true",
                    help="summarise the S2 Phase 0 `loadall slow`/`loadall stats` "
                         "lines instead of the per-frame MINI_FRAMELOG")
    ap.add_argument("--texload", action="store_true",
                    help="summarise historical TXD load profiler `texload slow`/"
                         "`texload stats` lines instead of the per-frame MINI_FRAMELOG")
    ap.add_argument("--prof", action="store_true",
                    help="summarise the frame profiler `prof:` aggregate/ring lines "
                         "(boot log or profile_frames.txt) with a ranked attribution")
    ap.add_argument("--prof-slow", action="store_true",
                    help="summarise the frame profiler slow-frame lines "
                         "(profile_slow.txt)")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    if not args.logs:
        ap.error("provide one log (summary) or two (A/B), or --self-test")

    if args.prof or args.prof_slow:
        if len(args.logs) == 1:
            describe_prof(args.logs[0], parse_prof(args.logs[0], after=args.after))
            return 0
        da = parse_prof(args.logs[0], after=args.after)
        ma = describe_prof(args.logs[0], da)
        print()
        db = parse_prof(args.logs[1], after=args.after)
        mb = describe_prof(args.logs[1], db)
        print()
        if ma is not None and mb is not None:
            print("  A/B median delta: %+.1f ms" % (mb - ma))
        for n in PROF_OUTER_SLOTS + PROF_SUB_SLOTS + PROF_GAME_SLOTS:
            if da["frames"] and db["frames"]:
                d = _slot_mean(db["frames"], n) - _slot_mean(da["frames"], n)
                if abs(d) >= 0.05:
                    print("  delta %-10s %+.1f ms" % (n, d))
        return 0

    if args.texload:
        if len(args.logs) == 1:
            describe_texload(args.logs[0], parse_texload(args.logs[0], after=args.after))
            return 0
        a = describe_texload(args.logs[0], parse_texload(args.logs[0], after=args.after))
        print()
        b = describe_texload(args.logs[1], parse_texload(args.logs[1], after=args.after))
        if a and b:
            print()
            print("  delta     : n %+d  readMs %+d  decodeMs %+d  fmtMs %+d  "
                  "createMs %+d  convMs %+d  uploadMs %+d"
                  % (b["n"] - a["n"], b["readMs"] - a["readMs"],
                     b["decodeMs"] - a["decodeMs"], b["fmtMs"] - a["fmtMs"],
                     b["createMs"] - a["createMs"], b["convMs"] - a["convMs"],
                     b["uploadMs"] - a["uploadMs"]))
        return 0

    if args.loadall:
        if len(args.logs) == 1:
            describe_loadall(args.logs[0], parse_loadall(args.logs[0], after=args.after))
            return 0
        a = describe_loadall(args.logs[0], parse_loadall(args.logs[0], after=args.after))
        print()
        b = describe_loadall(args.logs[1], parse_loadall(args.logs[1], after=args.after))
        if a and b:
            print()
            print("  delta     : ioMs %+d  cpuMs %+d  worstMs %+d"
                  % (b["ioMs"] - a["ioMs"], b["cpuMs"] - a["cpuMs"],
                     b["worstMs"] - a["worstMs"]))
        return 0

    if len(args.logs) == 1:
        describe(args.logs[0], parse(args.logs[0], after=args.after))
        return 0

    a = parse(args.logs[0], after=args.after)
    b = parse(args.logs[1], after=args.after)
    p95a = describe(args.logs[0], a)
    print()
    p95b = describe(args.logs[1], b)
    print()
    # Acceptance: fewer windows above the baseline p95 with the async build.
    slow_a = over_95th(a["frame"], p95a)
    slow_b = over_95th(b["frame"], p95a)
    print("A/B (baseline p95 = %.1f ms): windows above -> A %d, B %d"
          % (p95a, slow_a, slow_b))
    print("  median delta : %+.1f ms   p95 delta : %+.1f ms   draw delta : %+.1f ms"
          % (statistics.median(b["frame"]) - statistics.median(a["frame"]),
             p95b - p95a, b["draw"] - a["draw"]))
    if a.get("im2d") is not None and b.get("im2d") is not None:
        print("  im deltas    : im2d %+.1f ms   im3d %+.1f ms"
              % (b["im2d"] - a["im2d"], b["im3d"] - a["im3d"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
