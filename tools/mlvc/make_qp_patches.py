#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""从多 QP 的 .rknn 目录生成 QPP1 补丁（自检：应用后与目标 diff=0）。

    python3 tools/mlvc/make_qp_patches.py \\
        --models-dir models/mlvc/rk3588_qp_models --base-qp 21 --out-dir models/mlvc/qp_patches
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_DIR = Path(__file__).resolve().parent
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))

import qppatch  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="从 qpXX/*.rknn 生成 enc_qpN.qppatch / dec_qpN.qppatch")
    p.add_argument("--models-dir", type=Path, required=True, help="含 qp13/ qp21/ … 子目录的模型树")
    p.add_argument("--out-dir", type=Path, default=Path("models/mlvc/qp_patches"))
    p.add_argument("--base-qp", type=int, default=21)
    p.add_argument("--coalesce-gap", type=int, default=qppatch.DEFAULT_COALESCE_GAP,
                   help="相邻差异区间之间小于等于该字节数则合并（默认 64）")
    p.add_argument("--enc-only", action="store_true")
    p.add_argument("--dec-only", action="store_true")
    args = p.parse_args(argv)
    parts: list[str]
    if args.enc_only and args.dec_only:
        print("不能同时 --enc-only 与 --dec-only", file=sys.stderr)
        return 2
    if args.enc_only:
        parts = ["enc"]
    elif args.dec_only:
        parts = ["dec"]
    else:
        parts = ["enc", "dec"]
    print(f"基座 qp={args.base_qp}  输出 {args.out_dir.resolve()}")
    qppatch.generate_from_qp_dir(
        args.models_dir,
        args.out_dir,
        base_qp=args.base_qp,
        gap=args.coalesce_gap,
        parts=parts,
    )
    print(f"清单: {args.out_dir / 'qppatch_manifest.json'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except qppatch.QppatchError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
