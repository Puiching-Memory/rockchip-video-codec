#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""MLVC 图边界算子外提：准备 ONNX、转 RKNN、编译并跑上板 A/B。

实验只保留同时满足的结果：

1. 输出 1:1（逐字节）
2. 端到端（含 CPU 外提）比基线真的更快
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

_DIR = Path(__file__).resolve().parent
ROOT = _DIR.parents[1]
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))

import rknn_convert  # noqa: E402
from onnx_extract import extract_file  # noqa: E402
from onnx_rewrite import prepare_onnx  # noqa: E402

DEFAULT_ONNX = (
    ROOT
    / ".build/deps/mlvc/video/output/models/dmc61sbr_reglu-mlvc-psnr-v1/onnx-generic/640x368"
)
DEFAULT_EXP = ROOT / ".build/mlvc_split_exp"
BENCH_SRC = _DIR / "rknn_split_bench.c"


def _run(cmd: list[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=cwd)


def cmd_prepare(onnx_dir: Path, exp: Path, qp: int) -> None:
    exp.mkdir(parents=True, exist_ok=True)
    enc = onnx_dir / "MLVCEncoder.onnx"
    dec = onnx_dir / "MLVCDecoder.onnx"
    if not enc.is_file() or not dec.is_file():
        raise SystemExit(f"缺少 ONNX: {enc} / {dec}")

    print("== prepare encoder baseline (fold qp, no rewrite) ==")
    info, report = prepare_onnx(enc, exp / "enc_base.onnx", qp=qp, rewrite=False, fold=True)
    print(f"  fold={report.folded_inputs} inputs={[t.shape for t in info.inputs]}")

    print("== extract encoder SpaceToDepth ==")
    enc_ex = extract_file(exp / "enc_base.onnx", exp / "enc_std.onnx", "space_to_depth")
    print(
        f"  SpaceToDepth bs={enc_ex.blocksize} {enc_ex.in_shape} → {enc_ex.out_shape} {enc_ex.notes}"
    )

    print("== prepare decoder baseline ==")
    info, report = prepare_onnx(dec, exp / "dec_base.onnx", qp=qp, rewrite=False, fold=True)
    print(f"  fold={report.folded_inputs} outputs={[t.shape for t in info.outputs]}")

    print("== extract decoder trailing DepthToSpace+Clip ==")
    dec_ex = extract_file(exp / "dec_base.onnx", exp / "dec_d2s.onnx", "depth_to_space")
    print(
        f"  DepthToSpace bs={dec_ex.blocksize} {dec_ex.in_shape} "
        f"clip=[{dec_ex.clip_lo},{dec_ex.clip_hi}] {dec_ex.notes}"
    )
    meta = {
        "qp": qp,
        "enc_base": str(exp / "enc_base.onnx"),
        "enc_std": str(exp / "enc_std.onnx"),
        "dec_base": str(exp / "dec_base.onnx"),
        "dec_d2s": str(exp / "dec_d2s.onnx"),
        "enc_blocksize": enc_ex.blocksize,
        "enc_in_shape": enc_ex.in_shape,
        "enc_out_shape": enc_ex.out_shape,
        "dec_blocksize": dec_ex.blocksize,
        "dec_in_shape": dec_ex.in_shape,
        "dec_out_shape": dec_ex.out_shape,
        "dec_clip": [dec_ex.clip_lo, dec_ex.clip_hi],
    }
    (exp / "extract_meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(f"  meta {exp / 'extract_meta.json'}")


def cmd_convert(exp: Path, platform: str, verbose: bool) -> None:
    rknn_convert.require_rknn()
    names = ("enc_base", "enc_std", "dec_base", "dec_d2s")
    for name in names:
        src = exp / f"{name}.onnx"
        dst = exp / f"{name}.rknn"
        if not src.is_file():
            raise SystemExit(f"先跑 prepare: 缺少 {src}")
        print(f"== convert {name} ==")
        produced = rknn_convert.convert_onnx_to_rknn(
            src, dst, target=platform, verbose=verbose
        )
        print(f"  → {produced} ({produced.stat().st_size} B)")


def cmd_compile(exp: Path) -> Path:
    exp.mkdir(parents=True, exist_ok=True)
    out = exp / "rknn_split_bench"
    _run(
        [
            "gcc",
            "-O2",
            "-std=c11",
            "-Wall",
            "-Wextra",
            str(BENCH_SRC),
            "-lrknnrt",
            "-lpthread",
            "-lm",
            "-o",
            str(out),
        ]
    )
    return out


def cmd_bench(exp: Path, warmup: int, frames: int) -> None:
    bench = exp / "rknn_split_bench"
    if not bench.is_file():
        bench = cmd_compile(exp)
    results = {}
    jobs = [
        (
            "enc_std",
            [
                str(bench),
                "--exp",
                "enc_std",
                "--a",
                str(exp / "enc_base.rknn"),
                "--b",
                str(exp / "enc_std.rknn"),
                "--warmup",
                str(warmup),
                "--frames",
                str(frames),
            ],
        ),
        (
            "dec_d2s",
            [
                str(bench),
                "--exp",
                "dec_d2s",
                "--a",
                str(exp / "dec_base.rknn"),
                "--b",
                str(exp / "dec_d2s.rknn"),
                "--warmup",
                str(warmup),
                "--frames",
                str(frames),
            ],
        ),
        (
            "custom_std_prod",
            [
                str(bench),
                "--exp",
                "custom_std",
                "--a",
                str(ROOT / "models/MLVCEncoder_rk3588.rknn"),
                "--warmup",
                str(warmup),
                "--frames",
                str(frames),
            ],
        ),
    ]
    for name, cmd in jobs:
        a_path = Path(cmd[cmd.index("--a") + 1])
        if "--b" in cmd:
            b_path = Path(cmd[cmd.index("--b") + 1])
            if not a_path.is_file() or not b_path.is_file():
                print(f"skip {name}: missing rknn")
                continue
        elif not a_path.is_file():
            print(f"skip {name}: missing {a_path}")
            continue
        print(f"== bench {name} ==")
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        results[name] = {
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
        }
        if proc.returncode != 0:
            print(f"  FAIL rc={proc.returncode}")
    (exp / "bench_results.json").write_text(json.dumps(results, indent=2) + "\n")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--onnx-dir", type=Path, default=DEFAULT_ONNX)
    p.add_argument("--exp-dir", type=Path, default=DEFAULT_EXP)
    p.add_argument("--qp", type=int, default=21)
    p.add_argument("--platform", default="rk3588")
    p.add_argument("--warmup", type=int, default=10)
    p.add_argument("--frames", type=int, default=40)
    p.add_argument("--verbose", action="store_true")
    p.add_argument(
        "stage",
        nargs="*",
        default=["prepare"],
        help="prepare | convert | compile | bench | all",
    )
    args = p.parse_args()
    stages = args.stage
    if "all" in stages:
        stages = ["prepare", "convert", "compile", "bench"]
    for st in stages:
        if st == "prepare":
            cmd_prepare(args.onnx_dir, args.exp_dir, args.qp)
        elif st == "convert":
            cmd_convert(args.exp_dir, args.platform, args.verbose)
        elif st == "compile":
            cmd_compile(args.exp_dir)
        elif st == "bench":
            cmd_bench(args.exp_dir, args.warmup, args.frames)
        else:
            raise SystemExit(f"未知 stage: {st}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
