#!/usr/bin/env python3
"""RD 基准 CSV 迁移、合并与追加写入。"""

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆

from __future__ import annotations

import csv
import sys
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path

CSV_FIELDS = [
    "codec",
    "target_kbps",
    "actual_kbps",
    "psnr_y",
    "psnr_u",
    "psnr_v",
    "psnr_avg",
    "ssim",
    "encode_sec",
    "decode_sec",
    "rga_sec",
    "write_sec",
    "postproc_sec",
]


def norm_row(row: dict) -> dict:
    row.setdefault("postproc_sec", "0.0")
    row.setdefault("rga_sec", "0.0")
    row.setdefault("write_sec", "0.0")
    post = float(row.get("postproc_sec") or 0)
    rga = float(row.get("rga_sec") or 0)
    wr = float(row.get("write_sec") or 0)
    if post <= 0 and (rga > 0 or wr > 0):
        post = rga + wr
    elif rga <= 0 and wr <= 0 and post > 0:
        row["rga_sec"] = f"{post:.3f}"
    row["postproc_sec"] = f"{post:.3f}"
    row["rga_sec"] = f"{rga:.3f}"
    row["write_sec"] = f"{wr:.3f}"
    return {k: row.get(k, "") for k in CSV_FIELDS}


def migrate_csv(path: Path) -> None:
    rows = list(csv.DictReader(path.open(newline="")))
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        for row in rows:
            row.setdefault("postproc_sec", "0.0")
            post = float(row.get("postproc_sec") or 0)
            row.setdefault("rga_sec", f"{post:.3f}" if post else "0.0")
            row.setdefault("write_sec", "0.0")
            w.writerow({k: row.get(k, "") for k in CSV_FIELDS})


def finalize_csv(
    main_path: Path,
    workdir: Path,
    mode: str,
    session_meta_path: Path,
    session_codecs_path: Path,
) -> None:
    partials = sorted(workdir.glob("results_*.csv"))
    if not partials:
        raise SystemExit(f"[error] 无分片 CSV: {workdir}/results_*.csv")

    by_key = OrderedDict()
    codecs = set()
    for partial in partials:
        with partial.open(newline="") as f:
            for row in csv.DictReader(f):
                norm = norm_row(row)
                key = (norm["codec"], norm["target_kbps"])
                by_key[key] = norm
                codecs.add(norm["codec"])

    new_rows = list(by_key.values())
    new_rows.sort(key=lambda r: (r["codec"], float(r["target_kbps"])))

    preserved = []
    if mode == "accumulate" and main_path.is_file() and main_path.stat().st_size > 0:
        with main_path.open(newline="") as f:
            for row in csv.DictReader(f):
                norm = norm_row(row)
                key = (norm["codec"], norm["target_kbps"])
                if key not in by_key:
                    preserved.append(norm)

    out_rows = preserved + new_rows
    out_rows.sort(key=lambda r: (r["codec"], float(r["target_kbps"])))
    all_codecs = codecs | {r["codec"] for r in preserved}
    tmp = main_path.with_suffix(".csv.tmp")
    with tmp.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        w.writerows(out_rows)
    tmp.replace(main_path)

    session_codecs_path.write_text("\n".join(sorted(all_codecs)) + "\n")

    meta = {}
    if session_meta_path.is_file():
        for line in session_meta_path.read_text().splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    meta["csv_mode"] = mode
    meta["codecs"] = ",".join(sorted(all_codecs))
    meta["rows"] = str(len(out_rows))
    meta["finalized_at"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    session_meta_path.write_text(
        "\n".join(f"{k}={v}" for k, v in meta.items()) + "\n"
    )

    if mode == "session":
        print(
            f"[csv] session 模式: {len(new_rows)} 行, {len(codecs)} 个 codec（无历史保留）"
        )
    else:
        print(
            f"[csv] accumulate 模式: 保留 {len(preserved)} 行历史 + {len(new_rows)} 行新数据"
        )


def write_row(
    csv_path: str,
    codec: str,
    tgt: str,
    br: str,
    q: str,
    t0: str,
    t1: str,
    t2: str,
    t3: str = "",
) -> None:
    enc = float(t1) - float(t0)
    dec = float(t2) - float(t1)
    post = float(t3) - float(t2) if t3 else 0.0
    Path(csv_path).open("a").write(
        f"{codec},{tgt},{br},{q},{enc:.1f},{dec:.1f},0.000,0.000,{post:.3f}\n"
    )


def write_session_row(
    csv_path: str,
    codec: str,
    tgt: str,
    br: str,
    q: str,
    t0: str,
    t1: str,
    dec: str,
    rga: str,
    wr: str,
    post: str,
) -> None:
    enc = float(t1) - float(t0)
    Path(csv_path).open("a").write(
        f"{codec},{tgt},{br},{q},{enc:.1f},{float(dec):.3f},"
        f"{float(rga):.3f},{float(wr):.3f},{float(post):.3f}\n"
    )


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(
            "usage: csv_io.py {migrate|finalize|write-row|write-session-row} ...",
            file=sys.stderr,
        )
        return 2

    cmd = argv[1]
    if cmd == "migrate":
        migrate_csv(Path(argv[2]))
        return 0
    if cmd == "finalize":
        finalize_csv(
            Path(argv[2]),
            Path(argv[3]),
            (argv[4] or "session").strip().lower(),
            Path(argv[5]),
            Path(argv[6]),
        )
        return 0
    if cmd == "write-row":
        write_row(*argv[2:10], argv[10] if len(argv) > 10 else "")
        return 0
    if cmd == "write-session-row":
        write_session_row(*argv[2:13])
        return 0

    print(f"unknown command: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
