#!/usr/bin/env python3
"""解析 ffmpeg psnr/ssim 统计输出。"""

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆

from __future__ import annotations

import math
import re
import statistics
import sys
from pathlib import Path


def parse_quality(stats_path: str | Path) -> str:
    stats = Path(stats_path)
    log_path = stats.with_suffix(".log")
    psnr_path = stats.with_suffix(".psnr")
    ssim_path = stats.with_suffix(".ssim")

    psnr = _psnr_from_stats_file(psnr_path) or _psnr_from_log(log_path)
    if not psnr:
        raise SystemExit(f"quality parse failed: {stats}")

    ssim = _ssim_from_stats_file(ssim_path)
    if ssim is None:
        ssim = _ssim_from_log(log_path)
    if ssim is None:
        raise SystemExit(f"ssim parse failed: {stats}")

    return ",".join(f"{v:.6f}" for v in (*psnr, ssim))


def _psnr_from_stats_file(psnr_path: Path):
    if not psnr_path.is_file():
        return None
    ys, us, vs, avgs = [], [], [], []
    for line in psnr_path.read_text().splitlines():
        m = re.search(
            r"psnr_avg:([\d.]+)\s+.*psnr_y:([\d.]+)\s+.*psnr_u:([\d.]+)\s+.*psnr_v:([\d.]+)",
            line,
        )
        if not m:
            continue
        avg, y, u, v = (float(x) for x in m.groups())
        if not all(math.isfinite(x) for x in (avg, y, u, v)):
            continue
        ys.append(y)
        us.append(u)
        vs.append(v)
        avgs.append(avg)
    if not ys:
        return None
    return (
        statistics.mean(ys),
        statistics.mean(us),
        statistics.mean(vs),
        statistics.mean(avgs),
    )


def _psnr_from_log(log_path: Path):
    if not log_path.is_file():
        return None
    log = log_path.read_text()
    psnr_m = re.findall(
        r"PSNR y:([\d.]+) u:([\d.]+) v:([\d.]+) average:([\d.]+)", log
    )
    if not psnr_m:
        return None
    y, u, v, avg = psnr_m[-1]
    return float(y), float(u), float(v), float(avg)


def _ssim_from_stats_file(ssim_path: Path):
    if not ssim_path.is_file():
        return None
    vals = []
    for line in ssim_path.read_text().splitlines():
        m = re.search(r"All:([\d.]+)", line)
        if m:
            vals.append(float(m.group(1)))
    if not vals:
        return None
    return statistics.mean(vals)


def _ssim_from_log(log_path: Path):
    if not log_path.is_file():
        return None
    log = log_path.read_text()
    chunk = log.split("Parsed_ssim")[-1] if "Parsed_ssim" in log else log
    ssim_m = re.findall(r"All:([\d.]+)", chunk)
    if not ssim_m:
        return None
    return float(ssim_m[-1])


def main(argv: list[str]) -> int:
    if len(argv) < 3 or argv[1] != "parse":
        print("usage: quality.py parse STATS_BASENAME", file=sys.stderr)
        return 2
    print(parse_quality(argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
