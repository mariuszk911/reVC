#!/usr/bin/env python3
"""Assemble the AmigaOS test overlay for a render platform.

The output directory (`reVc_pistorm3d`) is an overlay for a GTA Vice City (PC,
v1.0) installation: the reVC binary, the minigl.library runtime, and the
`gamefiles/` data overrides reVC ships. See FILES.txt and INSTALL.txt in the
output directory.

Used by build.py (default) and by `make stage` in build/amigaos/Makefile.
"""

import argparse
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# Files a full GTA Vice City PC installation must provide. Not shipped here.
# Derived from the actual loads: models/GTA3.IMG + anim/PED.IFP (CdStream/
# LoadAnimFile) and data/gta_vc.dat + data/default.dat (CFileLoader::LoadLevel).
REQUIRED_GAME_FILES = [
    "models/GTA3.IMG         (all models/textures)",
    "models/                 (any other loose .txd/.dff/.img)",
    "data/                   (gta_vc.dat, default.dat + referenced .ide/.ipl/.zon/.cfg/.col)",
    "anim/                   (IFP animations, e.g. PED.IFP)",
    "TEXT/<language>.gxt     (american.gxt and the other languages are provided by this overlay)",
]
REQUIRED_GAME_NOTE = (
    "A complete GTA Vice City (PC, v1.0) installation is required. The game "
    "refuses to start unless data/gta_vc.dat and models/gta3.img exist. "
    "movies/ is not needed (NO_MOVIES); audio/ (sfx.RAW + sfx.SDT) is used by "
    "the AHI SFX backend, which is off by default (opt in with REVC_AHI=1); "
    "models/TXD.IMG is not used (TXD streaming is disabled for "
    "AmigaOS - TXDs are loaded directly instead).")


def _copy(outdir, src, rel, staged):
    if not os.path.isfile(src):
        return False
    dst = os.path.join(outdir, rel)
    dstdir = os.path.dirname(dst)
    if dstdir:
        os.makedirs(dstdir, exist_ok=True)
    shutil.copy2(src, dst)
    staged.append(rel)
    return True


def _sdk_minigl_version(sdk_root):
    """MINIGL_VERSION from the SDK's libraries/minigl.h (e.g. "29"), or ""."""
    path = os.path.join(sdk_root, "minigl-shared-library", "include",
                        "libraries", "minigl.h")
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("#define MINIGL_VERSION"):
                    return line.split()[-1].strip()
    except OSError:
        pass
    return ""


def _library_version(path):
    """The `$VER:` version (e.g. "29.1") embedded in a minigl.library, or ""."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return ""
    tag = b"minigl.library "
    i = data.find(tag)
    if i < 0:
        return ""
    out = bytearray()
    for ch in data[i + len(tag):i + len(tag) + 16]:
        if 48 <= ch <= 57 or ch == 46:	# digits and '.'
            out.append(ch)
        else:
            break
    return out.decode("ascii", "ignore")


def stage(render, outdir, binpath, bin_db, gamefiles,
          pistorm3d=None, quiet=False):
    """Stage the overlay. `outdir` is NOT wiped (a tester may keep game data)."""
    os.makedirs(outdir, exist_ok=True)
    staged = []

    # 1) game binary (stripped) + symbol copy for crash/remote debugging
    _copy(outdir, binpath, "reVC", staged)
    _copy(outdir, bin_db, "reVC.db", staged)

    # 2) the minigl.library runtime
    lib = os.path.join(pistorm3d, "..", "bin", "minigl.library")
    if not _copy(outdir, lib, "minigl.library", staged):
        print("ERROR: the minigl.library runtime is missing at %s"
              % os.path.normpath(lib))
        print("       (the 29.x client fails silently without the matching "
              "library; see SDK/README.txt)")
        raise SystemExit(1)
    # The 29.x dispatch client refuses a mismatched library silently:
    # MiniGLOpen() returns FALSE with no diagnostic. Fail the build instead.
    want = _sdk_minigl_version(pistorm3d)
    got = _library_version(os.path.join(outdir, "minigl.library"))
    if want and got and got.split(".")[0] != want.split(".")[0]:
        print("ERROR: staged minigl.library is %s but SDK MINIGL_VERSION is %s; "
              "the client would refuse it silently (MiniGLOpen returns FALSE)."
              % (got, want))
        raise SystemExit(1)
    if got and not quiet:
        print("staged minigl.library %s" % got)

    # 3) the gamefiles/ overlay reVC ships (updated TXDs, GXT, neo tables, ...)
    if os.path.isdir(gamefiles):
        for root, _dirs, files in os.walk(gamefiles):
            for name in sorted(files):
                src = os.path.join(root, name)
                _copy(outdir, src, os.path.relpath(src, gamefiles), staged)
    elif not quiet:
        print("WARNING: gamefiles directory not found: %s" % gamefiles)

    # 4) FILES.txt - machine-readable file list + required originals
    lines = ["reVC AmigaOS 68k test overlay",
             "render platform : %s" % render,
             "staged files    : %d" % len(staged),
             "",
             "STAGED FILES (copy these over your GTA VC directory):"]
    for rel in staged:
        size = os.path.getsize(os.path.join(outdir, rel))
        lines.append("  %-42s %9d bytes" % (rel, size))
    lines += ["",
              "RUNTIME REQUIREMENTS:"]
    for r in [
        "minigl.library 29.x (staged here) in LIBS:",
        "a PiStorm/RTG/V3D graphics setup",
    ]:
        lines.append("  " + r)
    lines += ["",
              "REQUIRED FROM THE ORIGINAL GAME (not shipped):"]
    for req in REQUIRED_GAME_FILES:
        lines.append("  " + req)
    lines += ["", REQUIRED_GAME_NOTE, ""]
    with open(os.path.join(outdir, "FILES.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")

    # 5) INSTALL.txt - human instructions
    lib_note = ("This is the PiStorm3D build. Copy minigl.library to LIBS: "
                "(or a directory already on the LIBS: search path).")
    overlay_lib = ("  minigl.library - the MiniGL runtime "
                   "(render platform: pistorm3d)")
    install = "\n".join([
        "reVC - GTA Vice City for AmigaOS 68k (%s)" % render,
        "=" * 60,
        "",
        "This directory is an OVERLAY for a GTA Vice City (PC, v1.0) install.",
        "It is not a standalone game.",
        "",
        "1. Copy the original GTA Vice City PC files into this directory (or copy",
        "   this directory over your GTA VC installation):",
        "     models/   data/   TEXT/   anim/   (the whole game)",
        "   The game must find data/gta_vc.dat and models/gta3.img.",
        "",
        "2. " + lib_note,
        "   LIBS: can be a real drawer or an assign; WinUAE/Amiberry: use the",
        "   emulated Libs: path.",
        "",
        "2b. OPTIONAL streamed audio (in-car radio, ambience, cutscene music,",
        "    police/mission speech): install mpega.library as LIBS:mpega.library",
        "    (Aminet util/libs/mpega_library.lha, use the 040 or 040FPU build).",
        "    Without it the streams stay silent; SFX are unaffected.",
        "    Set REVC_AHI_MUSIC=0 to disable streams explicitly.",
        "",
        "3. From the game directory on the Amiga, run:",
        "     reVC",
        "   (Run from Shell so early startup errors are visible.)",
        "",
        "4. Output: reVC.log (next to the game); savegames/settings in the",
        "   userfiles/ directory created next to the game.",
        "",
        "Contents of this overlay:",
        "  reVC           - the game executable (stripped)",
        "  reVC.db        - same executable with symbols (crash/remote debugging)",
        overlay_lib,
        "  models/ neo/ TEXT/ data/ - updated/extra game data (TXDs, GXT, tables)",
        "  gamecontrollerdb.txt     - controller mappings (glfw builds)",
        "  FILES.txt      - file list + required originals",
        "",
        "See FILES.txt for the complete list and the required original files.",
        "",
    ])
    with open(os.path.join(outdir, "INSTALL.txt"), "w") as f:
        f.write(install)

    return staged


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--render", choices=("pistorm3d",), default="pistorm3d")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--bin", required=True, dest="binpath")
    ap.add_argument("--bin-db", default="")
    ap.add_argument("--gamefiles", default=os.path.join(ROOT, "gamefiles"))
    ap.add_argument("--pistorm3d", default=os.path.join(
        ROOT, "PiStorm3D", "SDK"))
    args = ap.parse_args()

    staged = stage(args.render, args.outdir, args.binpath,
                   args.bin_db or (args.binpath + ".db"), args.gamefiles,
                   pistorm3d=args.pistorm3d)
    print("staged %d file(s) into %s" % (len(staged), args.outdir))


if __name__ == "__main__":
    main()
