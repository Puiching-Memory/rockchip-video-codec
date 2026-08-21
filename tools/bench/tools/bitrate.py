#!/usr/bin/env python3
"""根据文件大小与时长计算实际码率（kbps）。"""

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆

from __future__ import annotations

import os
import sys


def actual_kbps(
    path: str,
    frames: str,
    duration_sec: float,
    fps_num: float,
    fps_den: float,
) -> str:
    size = os.path.getsize(path)
    fps = fps_num / fps_den if fps_den else 30.0
    dur = duration_sec
    if frames and frames != "0":
        dur = float(frames) / fps
    if dur <= 0:
        return "0"
    return f"{size * 8 / dur / 1000:.2f}"


def main(argv: list[str]) -> int:
    if len(argv) < 6:
        print(
            "usage: bitrate.py FILE FRAMES DURATION_SEC FPS_NUM FPS_DEN",
            file=sys.stderr,
        )
        return 2
    print(
        actual_kbps(
            argv[1],
            argv[2],
            float(argv[3]),
            float(argv[4]),
            float(argv[5]),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
