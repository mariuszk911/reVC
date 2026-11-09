#!/usr/bin/env python3
"""Classify AmigaOS target test logs.

Each cross-compiled test writes `DH11:<log>.log` and terminates with one of:
    "<name>: all ok"
    "<name>: N failure(s)"
    "<name>: SKIP (...)"
and prints `FAIL <check> ...` lines for individual failures.

A log with no terminal line (crash midway, truncated buffer) is MISSING/
TIMEOUT -- partial `ok` lines are never trusted as a pass.

Importable: `from parse_target_logs import classify, PASS, FAIL, SKIP, MISSING`.
CLI:        parse_target_logs.py [--name NAME] <logfile> [...]
            parse_target_logs.py --self-test    (host-only, no emulator)
"""
import argparse
import re
import sys
from dataclasses import dataclass

PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"
MISSING = "MISSING"


@dataclass
class Result:
    name: str
    status: str
    fails: int = 0
    oks: int = 0
    detail: str = ""


def _terminal_patterns(name):
    esc = re.escape(name)
    return {
        "ok": re.compile(r"^%s: all ok\s*$" % esc),
        "fail": re.compile(r"^%s: (\d+) failure\(s\)\s*$" % esc),
        "skip": re.compile(r"^%s: SKIP(.*)$" % esc),
    }


def classify(text, name=None):
    """Return a Result for one test's log text.

    If `name` is given, terminal lines are matched against it. Otherwise the
    first terminal-looking line of any test name is accepted.
    """
    lines = text.splitlines()
    fails = [ln for ln in lines if ln.startswith("FAIL ")]
    oks = [ln for ln in lines if ln.startswith("ok ")]

    term_ok = False
    term_fail = None
    term_skip = None

    if name:
        pats = _terminal_patterns(name)
        for ln in lines:
            if pats["fail"].match(ln):
                term_fail = ln
            elif pats["skip"].match(ln):
                term_skip = ln
            elif pats["ok"].match(ln):
                term_ok = True
    else:
        for ln in lines:
            m = re.match(r"^([^:]+): all ok\s*$", ln)
            if m:
                name = m.group(1).strip()
                term_ok = True
                continue
            m = re.match(r"^([^:]+): (\d+) failure\(s\)\s*$", ln)
            if m:
                name = m.group(1).strip()
                term_fail = ln
                continue
            m = re.match(r"^([^:]+): SKIP(.*)$", ln)
            if m:
                name = m.group(1).strip()
                term_skip = ln

    label = name or "?"

    if term_fail is not None:
        n = int(re.search(r"(\d+) failure", term_fail).group(1))
        return Result(label, FAIL, fails=n, oks=len(oks), detail=term_fail)
    if fails:
        return Result(label, FAIL, fails=len(fails), oks=len(oks),
                      detail=fails[0])
    if term_skip is not None:
        return Result(label, SKIP, oks=len(oks), detail=term_skip)
    if term_ok:
        return Result(label, PASS, oks=len(oks))
    return Result(label, MISSING, oks=len(oks),
                  detail="no terminal line (timeout/crash/truncated)")


def classify_file(path, name=None):
    try:
        with open(path, "r", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        return Result(name or path, MISSING, detail=str(exc))
    return classify(text, name)


_SELF_TESTS = [
    # (description, text, name, expected_status)
    ("pass", "endian_test: all ok\n", "endian_test", PASS),
    ("fail summary",
     "endian_test: start\nok a\nFAIL b (f:1)\nendian_test: 1 failure(s)\n",
     "endian_test", FAIL),
    ("fail detail only (crash after FAIL)",
     "endian_test: start\nFAIL b (f:1)\n", "endian_test", FAIL),
    ("skip", "asset_endian_test: SKIP (american.gxt not found)\n",
     "asset_endian_test", SKIP),
    ("partial ok is not a pass",
     "anglewrap_test: start\nok a\nok b\n", "anglewrap_test", MISSING),
    ("empty log", "", "endian_test", MISSING),
    ("name mismatch is missing",
     "other_test: all ok\n", "endian_test", MISSING),
    ("infer name from terminal",
     "anglewrap_test: all ok\n", None, PASS),
    ("infer fail name",
     "list_guard_test: 3 failure(s)\n", None, FAIL),
]


def self_test():
    bad = 0
    for desc, text, name, expected in _SELF_TESTS:
        got = classify(text, name).status
        mark = "ok  " if got == expected else "FAIL"
        if got != expected:
            bad += 1
        print("%s self-test %-40s expected=%s got=%s" % (mark, desc, expected, got))
    if bad:
        print("parse_target_logs: %d self-test failure(s)" % bad)
        return 1
    print("parse_target_logs: self-test all ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--name", help="expected test name for terminal lines")
    ap.add_argument("--self-test", action="store_true",
                    help="run host-only parser self-tests and exit")
    ap.add_argument("logs", nargs="*", help="log file(s) to classify")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.logs:
        ap.error("no log files given (or use --self-test)")

    if args.name and len(args.logs) > 1:
        ap.error("--name only applies to a single log file")

    rc = 0
    for path in args.logs:
        res = classify_file(path, args.name)
        if res.status in (FAIL, MISSING):
            rc = 1
        line = "%s %s" % (res.status, path)
        if res.detail:
            line += "  (%s)" % res.detail
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
