#!/usr/bin/env python3
"""Make-less build driver for the AmigaOS 68k port.

The published `amigadev/m68k-amigaos-gcc:latest` image ships the Bebbo GCC 6.5
toolchain but no `make`/`ninja` and has no network, so build/amigaos/Makefile
cannot run inside it. This driver mirrors that Makefile's source selection,
defines and flags and adds mtime-based incremental compilation, so the port can
be built in either environment.

Usage (inside the container, repository mounted at /src):
    python3 build/amigaos/build.py --compile-only
    python3 build/amigaos/build.py
    python3 build/amigaos/build.py --clean
    python3 build/amigaos/build.py --info
See --help for the overridable knobs (CPU, FPU, PISTORM3D, LIBRW_PLATFORM, jobs).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SKIP_DIRS = ("skel/generic", "skel/glfw", "skel/sdl2", "skel/win", "skel/android",
             "audio/oal", "audio/mss", "audio/eax")
SKIP_FILES = ("CdStream_posix.cpp", "CdStream_win.cpp", "CdStream_ee.c")

# GCC 6.5 sometimes crashes/miscompiles at -O2 with -malign-int; these files
# need a lower optimisation level (mirrors the Makefile's per-file rules).
OPT_OVERRIDES = {
    "src/save/GenericGameStorage.cpp": "-O1",
    "src/peds/Ped.cpp": "-O1",
    # GCC 6.5's -O2 byte-value dataflow is unsound on 68k: MOVE.B/CLR.B into a
    # data register do NOT clear the upper 24 bits, but the optimiser assumes
    # they do and skips the zero-extension. In MusicManager this made
    # UsesPoliceRadio/UsesTaxiRadio (bool8) and GetCarTuning (returns veh->
    # m_nRadioStation, a uint8) leak garbage into m_nFrontendTrack -- e.g.
    # 0x0B7C0005 instead of station 5 -- so in-car radio never started.
    "src/audio/MusicManager.cpp": "-O1",
    # Same 68k -O2 float/byte-dataflow codegen problem in the head IK and the
    # animation frame paths. Must stay in sync with the Makefile's per-file
    # rules, or the default driver silently compiles them at -O2.
    "src/peds/PedIK.cpp": "-O1",
    "src/animation/FrameUpdate.cpp": "-O1",
    "src/animation/RpAnimBlend.cpp": "-O1",
    "src/animation/AnimBlendNode.cpp": "-O1",
}

# Extra per-file flags. The libnix float-math overrides must be built with
# -fno-builtin, or gcc rewrites "(float)sin(x)" back into a call to sinf and
# the wrappers recurse into themselves.
EXTRA_CFLAGS = {
    "src/skel/amigaos/amiga_libfix.c": ["-fno-builtin"],
}

# amiga_libfix.c must always be compiled (-fno-builtin) and linked: the
# toolchain's soft-float *f functions return in d0 while hard-float callers
# read fp0, so without these overrides the matrices go to ~1e15. The link map
# is checked after every link; symbol presence is not enough (lib__m__.a
# defines the same names), so each *f must be DEFINED BY amiga_libfix.c.o.
LIBFIX_SYMS = (
    "sqrtf", "sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "atan2f",
    "floorf", "ceilf", "rintf", "fabsf", "fmodf", "powf", "expf", "logf",
    "log10f", "log2f", "round", "copysign",
)
LIBFIX_SECTION_RE = re.compile(r"^\s+\.[^\s]+\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+.*\.o\s*$")
LIBFIX_SYMBOL_RE = re.compile(r"^\s+0x[0-9a-fA-F]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")


def discover_revc_sources():
    src_root = os.path.join(ROOT, "src")
    out = []
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel = os.path.relpath(dirpath, src_root).replace(os.sep, "/")
        if any(rel == s or rel.startswith(s + "/") for s in SKIP_DIRS):
            dirnames[:] = []
            continue
        for name in filenames:
            if name.endswith((".cpp", ".c")) and name not in SKIP_FILES:
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def discover_librw_sources(platform):
    src = os.path.join(ROOT, "vendor", "librw", "src")
    out = [os.path.join(src, n) for n in sorted(os.listdir(src))
           if n.endswith(".cpp")]
    lodepng = os.path.join(src, "lodepng")
    if os.path.isdir(lodepng):
        out += [os.path.join(lodepng, n) for n in sorted(os.listdir(lodepng))
                if n.endswith(".cpp")]
    if platform == "MINI":
        mini = os.path.join(src, "mini")
        if os.path.isdir(mini):
            out += [os.path.join(mini, n) for n in sorted(os.listdir(mini))
                    if n.endswith(".cpp")]
    return out


def src_include_dirs():
    src_root = os.path.join(ROOT, "src")
    dirs = [os.path.join(src_root, "fakerw")]
    for dirpath, _dirnames, _filenames in os.walk(src_root):
        dirs.append(dirpath)
    dirs.append(os.path.join(ROOT, "vendor", "librw"))
    dirs.append(os.path.join(ROOT, "vendor", "librw", "src"))
    return dirs


def verify_libfix(map_path, label=""):
    """Fail the build if amiga_libfix.c's *f overrides are not the defining
    object in the linker map (a plain symbol check would also match the
    toolchain's generic soft-float libm, lib__m__.a)."""
    try:
        with open(map_path, "r", errors="replace") as fh:
            lines = fh.readlines()
    except OSError as exc:
        print("verify-libfix: ERROR: cannot read %s (%s)" % (map_path, exc))
        return False
    defined = set()
    obj = None
    for line in lines:
        if LIBFIX_SECTION_RE.match(line):
            obj = line.split()[-1]
            continue
        m = LIBFIX_SYMBOL_RE.match(line)
        if m and obj and obj.endswith("amiga_libfix.c.o"):
            defined.add(m.group(1))
    missing = [s for s in LIBFIX_SYMS if s not in defined]
    if missing:
        print("verify-libfix: ERROR: libnix float overrides not defined by "
              "amiga_libfix.c.o: %s" % " ".join(missing))
        print("  amiga_libfix.c must be in the sources and built with "
              "-fno-builtin")
        return False
    soft = sorted({m for line in lines
                   for m in re.findall(r"lib__m__\.a\([^)]*\)", line)})
    if soft:
        print("verify-libfix: ERROR: soft-float lib__m__.a is linked -- a *f "
              "override is routing through a soft double (wrong ABI):")
        for m in soft:
            print("  " + m)
        return False
    print("verify-libfix: OK%s" % (" (%s)" % label if label else ""))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--compile-only", action="store_true",
                    help="compile every translation unit, do not link")
    ap.add_argument("--clean", action="store_true",
                    help="remove objects, generated files and linked "
                         "binaries before building")
    ap.add_argument("--info", action="store_true",
                    help="print the resolved build configuration and exit")
    ap.add_argument("--jobs", "-j", type=int,
                    default=os.cpu_count() or 4)
    ap.add_argument("--cpu", default=os.environ.get("CPU", "68040"))
    ap.add_argument("--fpu", default=os.environ.get("FPU", "68881"))
    ap.add_argument("--platform", default=os.environ.get("LIBRW_PLATFORM", "MINI"))
    ap.add_argument("--render", choices=("pistorm3d",),
                    default=os.environ.get("RENDER", "pistorm3d"),
                    help="MiniGL render platform; selects the SDK linked and "
                         "the staging directory reVc_<render>")
    ap.add_argument("--opt", default=os.environ.get("OPT", "-O2"))
    ap.add_argument("--lean", action="store_true",
                    default=os.environ.get("FP_LEAN", "").lower() in
                            ("1", "yes", "true", "on"),
                    help="compile out the detailed frame-profiler spans "
                         "(REVC_FP_LEAN); REVC_PROFILE=1 then reports frame/fps "
                         "only")
    ap.add_argument("--cross", default=os.environ.get("CROSS", "m68k-amigaos-"))
    ap.add_argument("--pistorm3d", default=os.environ.get("PISTORM3D",
                    os.path.join(ROOT, "PiStorm3D", "SDK")),
                    help="minigl.library 29.x client SDK root: expects "
                         "minigl-shared-library/include/ (headers, with backend/ a "
                         "sibling) and lib/libminigl.a (import library)")
    ap.add_argument("--gamefiles", default=os.environ.get("GAMEFILES",
                    os.path.join(ROOT, "gamefiles")),
                    help="overlay directory copied into the staging output")
    ap.add_argument("--outdir", default=os.environ.get("OUTDIR", ""),
                    help="staging directory (default <build>/reVc_<render>)")
    ap.add_argument("--no-stage", dest="stage", action="store_false", default=True,
                    help="do not assemble the reVc_<render> overlay")
    ap.add_argument("--stack", type=int, default=int(os.environ.get("STACK", "2097152")),
                    help="large task stack in bytes via libnix swapstack.o "
                         "(0 disables; default 2 MB)")
    ap.add_argument("--swapstack", default=os.environ.get("SWAPSTACK", ""),
                    help="override path to libnix swapstack.o")
    ap.add_argument("--libnix-fix", default=os.environ.get("LIBNIX_FIX", ""))
    args = ap.parse_args()

    cc = args.cross + "gcc"
    cxx = args.cross + "g++"

    defines = ["-DAMIGAOS", "-DBIGENDIAN", "-DLIBRW",
               "-DRW_%s" % args.platform, "-DUSE_OUR_VERSIONING",
               "-DNO_HOOK_MALLOC"]
    if args.lean:
        defines.append("-DREVC_FP_LEAN")
    # The PiStorm3D shared-library dispatch API (<proto/minigl.h>) is the only
    # MiniGL binding. See vendor/librw/src/mini/mglapi.h.
    # Do NOT add -malign-int: it grows AmigaOS exec/NDK structs (struct Node
    # 14->16) so Task/IntuiMessage/TagItem/... offsets no longer match the OS
    # ABI (this produced garbage tc_SPUpper values and Gurus). The toolchain
    # default (2-byte-aligned int) is the correct AmigaOS ABI.
    common = ["-m%s" % args.cpu, "-m%s" % args.fpu, "-mhard-float",
              "-mcrt=nix20",
              "-fno-omit-frame-pointer",
              "-fno-builtin-sin", "-fno-builtin-cos",
              "-fno-builtin-sinf", "-fno-builtin-cosf",
              "-Wall", "-Wno-unused", "-Wno-unused-parameter",
              "-Wno-sign-compare", "-Wno-reorder"]
    cflags = common + args.opt.split() + ["-std=gnu11"]
    cxxflags = common + args.opt.split() + ["-std=gnu++11", "-fno-exceptions",
                         "-fno-rtti", "-fno-threadsafe-statics",
                         "-fno-strict-aliasing"]

    # Object/generated dirs are per render platform: the two platforms differ
    # in defines/includes/link libs, and the driver only does mtime checks.
    gen = os.path.join(HERE, "gen", args.render)
    objdir = os.path.join(HERE, "obj", args.render)
    target = os.path.join(HERE, "reVC-%s" % args.render)
    target_db = target + ".db"

    revc_sources = discover_revc_sources()
    librw_sources = discover_librw_sources(args.platform)
    sources = revc_sources + librw_sources

    if args.info:
        outdir = args.outdir or os.path.join(HERE, "reVc_" + args.render)
        minigl_lib = os.path.join(args.pistorm3d, "lib", "libminigl.a")
        swap = args.swapstack
        if not swap and args.stack > 0:
            q = subprocess.run([cc, "-mcrt=nix20",
                                "-print-file-name=swapstack.o"],
                               capture_output=True, text=True)
            swap = q.stdout.strip()

        def status(path):
            return "found" if os.path.exists(path) else "MISSING"

        print("reVC AmigaOS build configuration")
        print("  render:       %s" % args.render)
        print("  librw:        %s" % args.platform)
        print("  cpu/fpu:      %s / %s (hard-float)" % (args.cpu, args.fpu))
        print("  optimisation: %s" % args.opt)
        print("  profiler:     %s" % ("lean (REVC_FP_LEAN)"
                                      if args.lean else "full (per-slot spans)"))
        print("  cross prefix: %s" % args.cross)
        print("  compiler:     %s" % (shutil.which(cxx) or "NOT FOUND"))
        print("  jobs:         %d" % args.jobs)
        print("  sources:      %d (%d game, %d librw)"
              % (len(sources), len(revc_sources), len(librw_sources)))
        print("  SDK:          %s" % args.pistorm3d)
        print("  minigl lib:   %s (%s)" % (minigl_lib, status(minigl_lib)))
        if args.stack > 0:
            print("  stack:        %d bytes" % args.stack)
            print("  swapstack.o:  %s (%s)" % (swap or "NOT FOUND",
                                               status(swap) if swap else "MISSING"))
        else:
            print("  stack:        disabled")
        print("  gamefiles:    %s (%s)" % (args.gamefiles, status(args.gamefiles)))
        print("  objects:      %s" % objdir)
        print("  generated:    %s" % gen)
        print("  binary:       %s" % target)
        print("  map:          %s.map" % target)
        print("  staging:      %s (%s)"
              % (outdir, "enabled" if args.stage else "disabled"))
        return

    if args.clean:
        # Fail loudly on permission errors: silently keeping stale objects
        # would turn the "clean" build into an incremental one.
        for d in (objdir, gen):
            if os.path.isdir(d):
                shutil.rmtree(d)
        if not args.compile_only:
            for path in (target, target_db, target + ".map"):
                try:
                    os.remove(path)
                except FileNotFoundError:
                    pass
        # The staged reVC/reVC.db are left alone: stage() overwrites them
        # after a successful link, and keeping the last good pair is safer if
        # this rebuild fails.

    # GL types/enums come from the PiStorm3D minigl.library 29.x SDK
    # (minigl-shared-library/include): OpenGL-valued GL_* enums and the
    # <proto/minigl.h> dispatch API.
    includes = ["-I" + d for d in src_include_dirs()]
    includes += ["-I" + gen,
                 "-I" + os.path.join(args.pistorm3d,
                                     "minigl-shared-library", "include")]

    os.makedirs(gen, exist_ok=True)

    # Rebuild key for the per-render object tree. Objects are otherwise chosen
    # by source mtime alone, so toggling --lean (or OPT) would relink stale
    # objects and produce a binary whose build id claims a configuration its
    # code was not compiled with. The stamp is only rewritten when the flags
    # change, so an unchanged configuration stays incremental. The name is
    # driver-specific because the Makefile shares this object directory and
    # keeps its own stamp: a shared file would make every driver switch rebuild.
    os.makedirs(objdir, exist_ok=True)
    config = "\n".join(
        defines + cflags + cxxflags + includes +
        ["%s=%s" % (rel, opt) for rel, opt in sorted(OPT_OVERRIDES.items())] +
        ["%s=%s" % (rel, " ".join(flags))
         for rel, flags in sorted(EXTRA_CFLAGS.items())])
    config_path = os.path.join(objdir, ".flags-buildpy")
    old_config = None
    if os.path.exists(config_path):
        try:
            with open(config_path) as fh:
                old_config = fh.read()
        except OSError:
            old_config = None
    if old_config != config:
        with open(config_path, "w") as fh:
            fh.write(config)
    config_mtime = os.path.getmtime(config_path)

    # Build id (GitSHA1.cpp): git commit if available, plus a content hash of the
    # sources and build flags -> the id changes on ANY edit and identifies the
    # exact binary (no generated/staged output is hashed, so it can't feed back).
    def _source_hash(paths, extra):
        h = hashlib.sha1()
        h.update(extra.encode())
        for p in sorted(paths):
            h.update(os.path.relpath(p, ROOT).encode())
            try:
                with open(p, "rb") as fh:
                    h.update(fh.read())
            except OSError:
                pass
        return h.hexdigest()[:10]

    try:
        git_sha = subprocess.check_output(
            ["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        git_sha = ""
    build_id = (git_sha + "." if git_sha else "") + _source_hash(
        sources + [os.path.join(ROOT, "build", "amigaos", "build.py"),
                   os.path.join(ROOT, "build", "amigaos", "Makefile")],
        " ".join(defines) + " " + args.opt)
    tpl = os.path.join(ROOT, "src", "extras", "GitSHA1.cpp.in")
    gensha = os.path.join(gen, "GitSHA1.cpp")
    with open(tpl) as f:
        text = f.read().replace("@GIT_SHA1@", build_id)
    if not os.path.exists(gensha) or open(gensha).read() != text:
        with open(gensha, "w") as f:
            f.write(text)
    gitsha_obj = os.path.join(objdir, "gen", "GitSHA1.o")

    # Generated splash logo: embed the PNG bytes for the AmigaOS boot splash
    # (no external asset at runtime). gen_splash owns the master image path.
    sys.path.insert(0, HERE)
    import gen_splash
    gensplash = os.path.join(gen, "splash_logo.cpp")
    splash_obj = os.path.join(objdir, "gen", "splash_logo.o")
    if not os.path.exists(gen_splash.DEFAULT_PNG):
        print("ERROR: splash logo not found: %s" % gen_splash.DEFAULT_PNG)
        print("       (it is embedded into the binary at build time)")
        sys.exit(1)
    if not os.path.exists(gensplash) or \
            os.path.getmtime(gensplash) < os.path.getmtime(gen_splash.DEFAULT_PNG) or \
            os.path.getmtime(gensplash) < os.path.getmtime(gen_splash.__file__):
        size = gen_splash.generate(gen_splash.DEFAULT_PNG, gensplash)
        print("gen_splash: embedded %d bytes -> %s" % (size, gensplash))

    def obj_for(src):
        rel = os.path.relpath(src, ROOT)
        return os.path.join(objdir, rel) + ".o"

    def compile_one(src):
        obj = obj_for(src)
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) \
                and os.path.getmtime(obj) >= config_mtime:
            return src, None
        is_c = src.endswith(".c")
        rel = os.path.relpath(src, ROOT).replace(os.sep, "/")
        opt = OPT_OVERRIDES.get(rel, args.opt)
        flags = list(cflags if is_c else cxxflags)
        if opt != args.opt:
            flags = [opt if f == args.opt else f for f in flags]
        flags += EXTRA_CFLAGS.get(rel, [])
        cmd = [cc if is_c else cxx]
        cmd += flags
        cmd += defines + includes + ["-c", src, "-o", obj]
        p = subprocess.run(cmd, capture_output=True, text=True)
        return src, (p.returncode, p.stderr)

    t0 = time.time()
    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for src, res in pool.map(compile_one, sources):
            if res is not None:
                rc, err = res
                if rc != 0:
                    failures.append((src, err))
                    print("FAILED %s" % os.path.relpath(src, ROOT))
                    sys.stdout.write(err)
                    sys.stdout.flush()

    if gitsha_obj not in (None,) and os.path.exists(
            os.path.join(ROOT, "src", "extras", "GitSHA1.cpp.in")):
        if not os.path.exists(gitsha_obj) or \
                os.path.getmtime(gitsha_obj) < os.path.getmtime(gensha):
            os.makedirs(os.path.dirname(gitsha_obj), exist_ok=True)
            p = subprocess.run([cxx] + cxxflags + defines + includes +
                               ["-c", gensha, "-o", gitsha_obj],
                               capture_output=True, text=True)
            if p.returncode != 0:
                failures.append((gensha, p.stderr))
                print("FAILED GitSHA1")
                sys.stdout.write(p.stderr)

    if not os.path.exists(splash_obj) or \
            os.path.getmtime(splash_obj) < os.path.getmtime(gensplash):
        os.makedirs(os.path.dirname(splash_obj), exist_ok=True)
        p = subprocess.run([cxx] + cxxflags + defines + includes +
                           ["-c", gensplash, "-o", splash_obj],
                           capture_output=True, text=True)
        if p.returncode != 0:
            failures.append((gensplash, p.stderr))
            print("FAILED splash_logo")
            sys.stdout.write(p.stderr)

    if failures:
        print("\n%d translation unit(s) failed (%.1fs)" % (len(failures),
                                                           time.time() - t0))
        sys.exit(1)

    print("compiled %d source(s) in %.1fs" % (len(sources), time.time() - t0))

    if args.compile_only:
        return

    # Large task stack. libnix's swapstack.o swaps to a stack whose size is the
    # global `__stack` longword (defined in a generated C file); the CLI/Shell
    # stack is far too small for reVC. Essential since the first on-target run
    # Guru'd during early startup.
    stack_objs = []
    if args.stack > 0:
        stack_c = os.path.join(gen, "amiga_stack.c")
        wanted = ("/* generated by build.py for libnix swapstack.o */\n"
                  "unsigned long __stack = %dUL;\n" % args.stack)
        if not os.path.exists(stack_c) or open(stack_c).read() != wanted:
            with open(stack_c, "w") as f:
                f.write(wanted)
        stack_obj = os.path.join(objdir, "gen", "amiga_stack.o")
        os.makedirs(os.path.dirname(stack_obj), exist_ok=True)
        p = subprocess.run([cc] + cflags + defines + includes +
                           ["-c", stack_c, "-o", stack_obj],
                           capture_output=True, text=True)
        if p.returncode != 0:
            print("FAILED amiga_stack.c")
            sys.stdout.write(p.stderr)
            sys.exit(1)
        stack_objs.append(stack_obj)

        swap = args.swapstack
        if not swap:
            q = subprocess.run([cc, "-mcrt=nix20", "-print-file-name=swapstack.o"],
                               capture_output=True, text=True)
            swap = q.stdout.strip()
        if not swap or not os.path.isfile(swap):
            print("ERROR: libnix swapstack.o not found; pass --swapstack=PATH "
                  "or --stack 0")
            sys.exit(1)
        stack_objs.append(swap)

    objs = [obj_for(s) for s in sources] + [gitsha_obj, splash_obj]
    extra = ([args.libnix_fix] if args.libnix_fix else []) + stack_objs
    ldflags = ["-m%s" % args.cpu, "-m%s" % args.fpu, "-mhard-float",
               "-mcrt=nix20",
               "-Wl,-Map=" + os.path.join(HERE, "reVC-%s.map" % args.render)]
    libs = ["-L" + os.path.join(args.pistorm3d, "lib"), "-lminigl", "-lm"]
    libfile = os.path.join(args.pistorm3d, "lib", "libminigl.a")
    if not os.path.isfile(libfile):
        print("ERROR: PiStorm3D import library not found: %s" % libfile)
        print("       Set --pistorm3d to the minigl.library 29.x SDK root "
              "(containing lib/ and minigl-shared-library/include/).")
        sys.exit(1)
    cmd = [cxx] + ldflags + ["-o", target] + objs + extra + libs
    p = subprocess.run(cmd)
    if p.returncode != 0:
        print("link failed")
        sys.exit(1)
    subprocess.run(["cp", "-f", target, target_db])
    # Never strip a linked binary: `strip` drops HUNK_RELOC32 on this toolchain.
    # Re-link with -s so the linker keeps relocations and omits the symbols.
    p = subprocess.run([cxx] + ldflags + ["-s", "-o", target] + objs + extra + libs)
    if p.returncode != 0 or not os.path.isfile(target) or os.path.getsize(target) == 0:
        print("strip link failed")
        sys.exit(1)
    # Keep the sidecar newer than the stripped binary: the Makefile driver
    # treats a newer .db as up to date and must not copy the stripped binary
    # over the symbols when it runs after this driver.
    os.utime(target_db, None)
    print("linked %s" % target)

    if not verify_libfix(os.path.join(HERE, "reVC-%s.map" % args.render),
                         args.render):
        sys.exit(1)

    if args.stage:
        from stage import stage
        outdir = args.outdir or os.path.join(HERE, "reVc_" + args.render)
        stage(args.render, outdir, target, target_db, args.gamefiles,
              pistorm3d=args.pistorm3d)
        print("staged into %s" % outdir)


if __name__ == "__main__":
    main()
