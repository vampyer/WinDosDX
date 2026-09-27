#!/usr/bin/env python3
"""Report whether a WAV recording holds sound, and when.

QEMU's wav audiodev (ntfs-regression.py --audio-wav) records everything the
guest plays. This prints the length, the peak level and the seconds that are
not silent, so a test can tell "the game played sound" from "silence".

    python wav-level.py capture.wav [--threshold 0.01]
"""

import argparse
import array
import struct
import sys


def read_wav(path):
    """Return (rate, channels, width, pcm). QEMU fills in the RIFF and data
    sizes only when it exits cleanly; a killed guest leaves them at 0, so
    a zero data size means "everything to the end of the file"."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a WAVE file")
    pos, fmt = 12, None
    while pos + 8 <= len(blob):
        tag, size = blob[pos:pos + 4], struct.unpack_from("<I", blob, pos + 4)[0]
        body = pos + 8
        if tag == b"fmt ":
            _, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", blob, body)
            fmt = (rate, channels, bits // 8)
        elif tag == b"data":
            if fmt is None:
                raise ValueError("data chunk before fmt chunk")
            end = len(blob) if size == 0 else min(len(blob), body + size)
            pcm = blob[body:end]
            return fmt + (pcm[:len(pcm) - len(pcm) % (fmt[1] * fmt[2])],)
        pos = body + size + (size & 1)
    raise ValueError("no data chunk")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wav")
    parser.add_argument("--threshold", type=float, default=0.01,
                        help="peak level (0-1) above which a second counts as sound")
    args = parser.parse_args()

    try:
        rate, channels, width, data = read_wav(args.wav)
    except (OSError, ValueError, struct.error) as e:
        print(f"{args.wav}: {e}")
        return 2
    frames = len(data) // (channels * width) if channels and width else 0
    if width != 2:
        print(f"{args.wav}: {width * 8}-bit samples are not supported")
        return 2
    samples = array.array("h", data)
    if sys.byteorder == "big":
        samples.byteswap()

    per_second = rate * channels
    loud = []
    peak = 0
    for start in range(0, len(samples), per_second):
        chunk = samples[start:start + per_second]
        if not chunk:
            break
        level = max(abs(min(chunk)), abs(max(chunk)))
        peak = max(peak, level)
        if level / 32768 > args.threshold:
            loud.append(start // per_second)

    seconds = frames / rate if rate else 0
    print(f"{args.wav}: {seconds:.1f} s, {rate} Hz, {channels} channel(s), "
          f"peak {peak / 32768:.3f}, sound in {len(loud)} of {int(seconds + 0.999)} seconds")
    if loud:
        ranges, begin = [], loud[0]
        for a, b in zip(loud, loud[1:] + [None]):
            if b != a + 1:
                ranges.append(f"{begin}-{a}" if a != begin else f"{a}")
                if b is not None:
                    begin = b
        print("  sound at seconds: " + ", ".join(ranges[:30]))
    return 0 if loud else 1


if __name__ == "__main__":
    sys.exit(main())
