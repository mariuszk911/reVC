#!/usr/bin/env python3
"""Host-side streamed-audio format check (plan 12.1 / 10.1).

The AHI stream bus feeds mpega.library through a bitstream hook that undoes the
GTA Vice City `.adf` obfuscation. This tool verifies, without an Amiga, that:

  * every Audio/*.adf is MPEG-1/2 Layer III with every byte XOR-ed with 0x22
    (raw: no valid frame chain; XOR 0x22: valid chain), and
  * every Audio/*.mp3 is a plain MPEG frame chain (no XOR),

and reports the first frame's sample rate / bitrate. It is the regression guard
for the format assumption behind the `STREAM_ADF_KEY` XOR in sampman_ahi.cpp.

Usage: tests/amigaos/check_audio_formats.py [AudioDir]
Default AudioDir: build/amigaos/reVc_pistorm3d/Audio
"""

import os
import sys

BITRATE = {
    1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
SAMPLE_RATE = {
    "mpeg1": [44100, 48000, 32000],
    "mpeg2": [22050, 24000, 16000],
    "mpeg25": [11025, 12000, 8000],
}


def parse_header(b, i):
    """Return (version, rate, kbps, frame_len_wo_pad, pad) or None."""
    if b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
        return None
    ver = (b[i + 1] >> 3) & 3          # 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
    layer = (b[i + 1] >> 1) & 3         # 1 = Layer III
    if ver == 1 or layer != 1:
        return None
    br = (b[i + 2] >> 4) & 0xF
    sri = (b[i + 2] >> 2) & 3
    pad = (b[i + 2] >> 1) & 1
    if br in (0, 0xF) or sri == 3:
        return None
    if ver == 3:
        rate, kbps, samples = SAMPLE_RATE["mpeg1"][sri], BITRATE[1][br], 1152
    elif ver == 2:
        rate, kbps, samples = SAMPLE_RATE["mpeg2"][sri], BITRATE[2][br], 576
    else:
        rate, kbps, samples = SAMPLE_RATE["mpeg25"][sri], BITRATE[2][br], 576
    frame_len = (samples // 8) * kbps * 1000 // rate
    return rate, kbps, frame_len, pad, samples


def frame_chain(b, limit=4):
    """Find a chain of >= `limit` consistent frames; return (offset, info)."""
    n = len(b)
    for i in range(0, min(n, 4 << 20) - 4):
        h = parse_header(b, i)
        if not h:
            continue
        rate, kbps, flen, pad, samples = h
        j = i
        ok = 0
        for _ in range(limit):
            if j + 4 > n:
                break
            hh = parse_header(b, j)
            if not hh or hh[0] != rate:
                break
            j += hh[2] + hh[3]
            ok += 1
        if ok >= limit:
            return i, (rate, kbps, samples)
    return None, None


def xor22(data):
    return bytes(c ^ 0x22 for c in data)


def check(path, want_xor):
    with open(path, "rb") as f:
        raw = f.read()
    head = raw[:4 << 20]
    if want_xor:
        off, info = frame_chain(xor22(head))
        raw_off, _ = frame_chain(head)
        if raw_off is not None:
            return False, "raw file also looks like MPEG (XOR assumption wrong)"
        if off is None:
            return False, "no MPEG frame chain after XOR 0x22"
        dur = len(raw) * 8 // info[1] // 1000 if info[1] else 0
        return True, "XOR-0x22 MPEG L3 %dHz %dkbps ~%ds" % (info[0], info[1], dur)
    else:
        off, info = frame_chain(head)
        if off is None:
            return False, "no MPEG frame chain (expected plain MP3)"
        return True, "MPEG L3 %dHz %dkbps" % (info[0], info[1])


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    audio = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "build", "amigaos", "reVc_pistorm3d", "Audio")
    if not os.path.isdir(audio):
        print("check_audio_formats: no such dir: %s" % audio)
        return 2

    fails = 0
    adf = sorted(f for f in os.listdir(audio) if f.lower().endswith(".adf"))
    mp3 = sorted(f for f in os.listdir(audio) if f.lower().endswith(".mp3"))
    for name in adf:
        ok, msg = check(os.path.join(audio, name), True)
        print(("PASS " if ok else "FAIL ") + name + ": " + msg)
        fails += 0 if ok else 1
    for name in mp3:
        ok, msg = check(os.path.join(audio, name), False)
        print(("PASS " if ok else "FAIL ") + name + ": " + msg)
        fails += 0 if ok else 1

    print("check_audio_formats: %d .adf + %d .mp3 checked, %d failure(s)"
          % (len(adf), len(mp3), fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
