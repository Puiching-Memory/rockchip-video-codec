#!/usr/bin/env python3
"""片段截取与源视频探测辅助。"""

from __future__ import annotations

import hashlib
import os
import sys


def clip_start(dur: float, clip_sec: float) -> str:
    if dur <= clip_sec:
        return "0"
    return f"{(dur - clip_sec) / 2:.3f}"


def duration_from_frames(fps_s: str, frames_n: int) -> float:
    if "/" in fps_s:
        num, den = fps_s.split("/", 1)
        fps = float(num) / float(den) if float(den) else 30.0
    else:
        fps = float(fps_s) if fps_s else 30.0
    if fps > 0 and frames_n > 0:
        return frames_n / fps
    return 0.0


def cache_key(path: str) -> str:
    return hashlib.sha256(path.encode()).hexdigest()[:16]


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: clip.py {start|cache-key|duration-from-frames} ...", file=sys.stderr)
        return 2

    cmd = argv[1]
    if cmd == "start":
        print(clip_start(float(argv[2] or 0), float(argv[3])))
        return 0
    if cmd == "cache-key":
        print(cache_key(argv[2]))
        return 0
    if cmd == "duration-from-frames":
        fps_s = os.environ.get("FPS_S", "30/1")
        frames_n = int(os.environ.get("FRAMES_N", "0") or 0)
        print(duration_from_frames(fps_s, frames_n))
        return 0

    print(f"unknown command: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
