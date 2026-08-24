#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""HEVC Class E 协议：MS-SSIM(dB)–CBR 扫点（bench 的第二种 RD 协议）。

与 ``run_rd_benchmark.sh``（任意片源、target kbps、ffmpeg PSNR/SSIM）共用
in-tree ffmpeg、rkvc_transcode、config.json 路径约定。本脚本只负责 Class E
序列 + CQP + Y 通道 5 尺度 MS-SSIM；绘图走 ``plot_rd_curve.py --preset hevc-e``。

默认把 HEVC-E 1280×720 源统一缩放到 640×368、取 96 帧。MLVC / MLVC-S 走
``rkvc_transcode -p neural``（RKNN）；MPP 在相同分辨率走 in-tree
``third_party/ffmpeg-rockchip/ffmpeg`` 的 h264_rkmpp / hevc_rkmpp CQP。
禁止使用系统 FFmpeg。
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import time
from pathlib import Path

_DIR = Path(__file__).resolve().parent
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))

from plot_rd_curve import plot_msssim_cbr  # noqa: E402
from tools.msssim import cbr_milli, mean_ms_ssim_y, msssim_db  # noqa: E402

ROOT = _DIR.parents[1]
IN_TREE_FFMPEG = ROOT / "third_party" / "ffmpeg-rockchip" / "ffmpeg"
RKVC_TRANSCODE = ROOT / ".build" / "release" / "rkvc_transcode"
MLVC_DEFAULT_W = 640
MLVC_DEFAULT_H = 368
SEQS = ("FourPeople", "Johnny", "KristenAndSara")
CSV_FIELDS = (
    "codec",
    "sequence",
    "qp",
    "bpp",
    "msssim",
    "msssim_db",
    "cbr_milli",
    "frames",
    "width",
    "height",
    "encode_sec",
)


def _run(cmd: list[str], *, cwd: Path | None = None, env: dict | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=cwd, env=env or _dep_env())


def _dep_env() -> dict[str, str]:
    env = os.environ.copy()
    ffmpeg_src = ROOT / "third_party" / "ffmpeg-rockchip"
    libs: list[Path] = [
        ROOT / ".build" / "deps" / "mpp-install" / "lib",
        ROOT / ".build" / "deps" / "librga-install" / "lib",
        ROOT / ".build" / "deps" / "svt-av1-install" / "lib",
        ROOT / ".build" / "deps" / "rknn-install" / "lib",
    ]
    for name in (
        "libavcodec",
        "libavformat",
        "libavutil",
        "libswscale",
        "libavfilter",
        "libavdevice",
        "libswresample",
        "libpostproc",
    ):
        libs.append(ffmpeg_src / name)
    extra = ":".join(str(p) for p in libs if p.is_dir())
    if extra:
        prev = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = extra + (":" + prev if prev else "")
    return env


def _is_system_ffmpeg(path: str) -> bool:
    resolved = str(Path(path).resolve())
    return resolved.startswith(("/usr/bin/", "/usr/local/bin/", "/bin/"))


def _ffmpeg() -> str:
    cand = os.environ.get("FFMPEG")
    if cand:
        if _is_system_ffmpeg(cand):
            raise SystemExit(
                f"禁止使用系统 FFmpeg ({cand})，会破坏可移植性。\n"
                f"  请用 {IN_TREE_FFMPEG}"
            )
        return cand
    if IN_TREE_FFMPEG.is_file() and os.access(IN_TREE_FFMPEG, os.X_OK):
        return str(IN_TREE_FFMPEG)
    raise SystemExit(
        f"未找到 in-tree ffmpeg: {IN_TREE_FFMPEG}\n"
        "  git submodule update --init --depth 1 third_party/ffmpeg-rockchip\n"
        "  ./scripts/rebuild-ffmpeg-rkmpp.sh\n"
        "禁止使用系统 FFmpeg（会破坏可移植性）。"
    )


def _rkvc_bin(args: argparse.Namespace) -> Path:
    p = Path(args.rkvc_bin) if args.rkvc_bin else RKVC_TRANSCODE
    if not p.is_file() or not os.access(p, os.X_OK):
        raise SystemExit(f"未找到 rkvc_transcode: {p}（先编 in-tree 依赖再 cmake --preset default）")
    return p


def clip_path(data_dir: Path, name: str, width: int, height: int) -> Path:
    return data_dir / f"{name}_{width}x{height}_60.yuv"


def prepare_clips(src_dir: Path, work: Path, width: int, height: int, frames: int) -> list[Path]:
    work.mkdir(parents=True, exist_ok=True)
    out = []
    for name in SEQS:
        native = clip_path(src_dir, name, 1280, 720)
        dest = clip_path(work, name, width, height)
        expect = width * height * 3 // 2 * frames
        if width == 1280 and height == 720 and native.is_file() and native.stat().st_size >= expect:
            dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.exists() or dest.is_symlink():
                dest.unlink()
            dest.symlink_to(native)
            print(f"[prep] 链接 {dest} → {native}")
            out.append(dest)
            continue
        if not native.is_file():
            raise SystemExit(f"缺少 Class E 源: {native}")
        if dest.is_file() and dest.stat().st_size >= expect:
            print(f"[prep] 已有 {dest}")
            out.append(dest)
            continue
        print(f"[prep] {name} 1280x720 → {width}x{height} × {frames}f")
        _run(
            [
                _ffmpeg(),
                "-y",
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "rawvideo",
                "-pix_fmt",
                "yuv420p",
                "-video_size",
                "1280x720",
                "-framerate",
                "60",
                "-i",
                str(native),
                "-frames:v",
                str(frames),
                "-vf",
                f"scale={width}:{height}:flags=lanczos",
                "-pix_fmt",
                "yuv420p",
                "-f",
                "rawvideo",
                str(dest),
            ]
        )
        out.append(dest)
    return out


def mlvc_model_dir(variant: str, args: argparse.Namespace) -> Path:
    if variant == "mlvc-s":
        return Path(args.mlvc_s_model_dir)
    return Path(args.mlvc_model_dir)


def resolve_mlvc_bundle(variant: str, args: argparse.Namespace) -> dict[str, Path]:
    root = mlvc_model_dir(variant, args)
    plat = args.mlvc_platform
    enc = root / f"MLVCEncoder_{plat}.rknn"
    dec = root / f"MLVCDecoder_{plat}.rknn"
    gauss = root / "gaussian.bin"
    bitest = root / "bitest.bin"
    missing = [p for p in (enc, dec, gauss, bitest) if not p.is_file()]
    if missing:
        if variant == "mlvc-s":
            export_hint = (
                f".venv/bin/python tools/mlvc/export_rknn.py --from-mlvc "
                f"--model-version dmc61sbr_reglu_s --weights-path /path/to/mlvc-s-psnr-v1.ckpt "
                f"--platform {plat} --out-dir {root}"
            )
        else:
            export_hint = (
                f".venv/bin/python tools/mlvc/export_rknn.py --from-mlvc "
                f"--platform {plat} --out-dir {root}"
            )
        raise SystemExit(
            f"{variant} 缺少 RKNN/PMF:\n  " + "\n  ".join(str(p) for p in missing) +
            f"\n请先导出: {export_hint}"
        )
    patch = root / "qp_patches"
    return {
        "enc": enc,
        "dec": dec,
        "gaussian": gauss,
        "bitest": bitest,
        "patch": patch if patch.is_dir() else None,  # type: ignore[dict-item]
    }


def scale_yuv420(
    src: Path, dest: Path, sw: int, sh: int, dw: int, dh: int, frames: int
) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    expect = dw * dh * 3 // 2 * frames
    if dest.is_file() and dest.stat().st_size >= expect:
        return
    _run(
        [
            _ffmpeg(),
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "yuv420p",
            "-video_size",
            f"{sw}x{sh}",
            "-framerate",
            "60",
            "-i",
            str(src),
            "-frames:v",
            str(frames),
            "-vf",
            f"scale={dw}:{dh}",
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(dest),
        ]
    )


def yuv_to_y4m(yuv: Path, y4m: Path, width: int, height: int, frames: int) -> None:
    if y4m.is_file() and y4m.stat().st_size > 0:
        return
    _run(
        [
            _ffmpeg(),
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "yuv420p",
            "-video_size",
            f"{width}x{height}",
            "-framerate",
            "60",
            "-i",
            str(yuv),
            "-frames:v",
            str(frames),
            "-f",
            "yuv4mpegpipe",
            str(y4m),
        ]
    )


def run_mlvc(variant: str, args: argparse.Namespace, data_dir: Path, work: Path) -> list[dict]:
    bundle = resolve_mlvc_bundle(variant, args)
    rkvc = str(_rkvc_bin(args))
    mw, mh = args.mlvc_width, args.mlvc_height
    rows: list[dict] = []
    for seq in SEQS:
        src = clip_path(data_dir, seq, args.width, args.height)
        if not src.is_file():
            raise SystemExit(f"缺少序列: {src}")
        scaled = work / variant / "clips" / f"{seq}_{mw}x{mh}_60.yuv"
        y4m = scaled.with_suffix(".y4m")
        scale_yuv420(src, scaled, args.width, args.height, mw, mh, args.frames)
        yuv_to_y4m(scaled, y4m, mw, mh, args.frames)
        for qp in args.mlvc_qp:
            ram = work / variant / seq / str(qp)
            ram.mkdir(parents=True, exist_ok=True)
            stream = ram / "stream.mlvc"
            rec = ram / "rec.yuv"
            print(f"[run] {variant} {seq} RKNN qp={qp}  {mw}x{mh}")
            enc_cmd = [
                rkvc,
                "-i",
                str(y4m),
                "-o",
                str(stream),
                "-p",
                "neural",
                "--mlvc-enc",
                str(bundle["enc"]),
                "--mlvc-gaussian-pmf",
                str(bundle["gaussian"]),
                "--mlvc-bitest-pmf",
                str(bundle["bitest"]),
                "--mlvc-qp",
                str(qp),
            ]
            if bundle["patch"] is not None:
                enc_cmd += ["--mlvc-qp-patch-dir", str(bundle["patch"])]
            t0 = time.time()
            _run(enc_cmd)
            enc_sec = time.time() - t0
            dec_cmd = [
                rkvc,
                "-i",
                str(stream),
                "-o",
                str(rec),
                "--mlvc-dec",
                str(bundle["dec"]),
                "--mlvc-gaussian-pmf",
                str(bundle["gaussian"]),
                "--mlvc-bitest-pmf",
                str(bundle["bitest"]),
            ]
            if bundle["patch"] is not None:
                dec_cmd += ["--mlvc-qp-patch-dir", str(bundle["patch"])]
            _run(dec_cmd)
            bits = stream.stat().st_size * 8
            bpp = bits / (mw * mh * args.frames)
            d = mean_ms_ssim_y(scaled, rec, mw, mh, args.frames)
            rec.unlink(missing_ok=True)
            rows.append(
                {
                    "codec": variant,
                    "sequence": seq,
                    "qp": str(qp),
                    "bpp": f"{bpp:.8f}",
                    "msssim": f"{d:.8f}",
                    "msssim_db": f"{msssim_db(d):.4f}",
                    "cbr_milli": f"{cbr_milli(bpp):.4f}",
                    "frames": str(args.frames),
                    "width": str(mw),
                    "height": str(mh),
                    "encode_sec": f"{enc_sec:.3f}",
                }
            )
            print(
                f"  bpp={bpp:.5f}  CBR×1e-3={cbr_milli(bpp):.2f}  "
                f"MS-SSIM={d:.4f} ({msssim_db(d):.2f} dB)"
            )
    return rows


def run_mpp(
    codec: str,
    args: argparse.Namespace,
    clips: list[Path],
    work: Path,
) -> list[dict]:
    enc = "h264_rkmpp" if codec == "h264" else "hevc_rkmpp"
    dec = enc
    rows = []
    for clip in clips:
        seq = clip.name.split("_")[0]
        ref = clip
        for qp in args.mpp_qp:
            ram = work / codec / seq / str(qp)
            ram.mkdir(parents=True, exist_ok=True)
            stream = ram / "stream.mp4"
            rec = ram / "rec.yuv"
            print(f"[run] {codec} {seq} CQP qp={qp}")
            t0 = time.time()
            _run(
                [
                    _ffmpeg(),
                    "-y",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-f",
                    "rawvideo",
                    "-pix_fmt",
                    "yuv420p",
                    "-video_size",
                    f"{args.width}x{args.height}",
                    "-framerate",
                    "60",
                    "-i",
                    str(ref),
                    "-frames:v",
                    str(args.frames),
                    "-c:v",
                    enc,
                    "-rc_mode",
                    "2",
                    "-qp_init",
                    str(qp),
                    "-g",
                    "60",
                    "-an",
                    str(stream),
                ]
            )
            enc_sec = time.time() - t0
            _run(
                [
                    _ffmpeg(),
                    "-y",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-c:v",
                    dec,
                    "-i",
                    str(stream),
                    "-fps_mode",
                    "passthrough",
                    "-frames:v",
                    str(args.frames),
                    "-pix_fmt",
                    "yuv420p",
                    "-f",
                    "rawvideo",
                    str(rec),
                ]
            )
            bits = stream.stat().st_size * 8
            bpp = bits / (args.width * args.height * args.frames)
            d = mean_ms_ssim_y(ref, rec, args.width, args.height, args.frames)
            rec.unlink(missing_ok=True)
            stream.unlink(missing_ok=True)
            rows.append(
                {
                    "codec": codec,
                    "sequence": seq,
                    "qp": str(qp),
                    "bpp": f"{bpp:.8f}",
                    "msssim": f"{d:.8f}",
                    "msssim_db": f"{msssim_db(d):.4f}",
                    "cbr_milli": f"{cbr_milli(bpp):.4f}",
                    "frames": str(args.frames),
                    "width": str(args.width),
                    "height": str(args.height),
                    "encode_sec": f"{enc_sec:.3f}",
                }
            )
            print(
                f"  bpp={bpp:.5f}  CBR×1e-3={cbr_milli(bpp):.2f}  "
                f"MS-SSIM={d:.4f} ({msssim_db(d):.2f} dB)"
            )
    return rows


def append_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not path.is_file()
    with path.open("a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if new_file:
            w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in CSV_FIELDS})


def load_csv(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def _hevc_e_cfg() -> dict:
    from tools.config import load_config

    raw = os.environ.get("BENCH_CONFIG")
    path = Path(raw) if raw else _DIR / "config.json"
    if not path.is_file():
        return {}
    return load_config(path, ROOT).get("hevc_e") or {}


def _csv_ints(vals, fallback: str) -> str:
    if not vals:
        return fallback
    return ",".join(str(x) for x in vals)


def build_parser() -> argparse.ArgumentParser:
    he = _hevc_e_cfg()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--src-dir", type=Path, default=Path(he.get("src_dir") or "/dev/shm/hevc-e"))
    p.add_argument("--out-dir", type=Path, default=_DIR / "results" / "hevc_e")
    p.add_argument("--width", type=int, default=int(he.get("width") or MLVC_DEFAULT_W))
    p.add_argument("--height", type=int, default=int(he.get("height") or MLVC_DEFAULT_H))
    p.add_argument("--frames", type=int, default=int(he.get("frames") or 96))
    p.add_argument(
        "--codecs",
        default=",".join(he.get("codecs") or ["h264", "h265", "mlvc", "mlvc-s"]),
    )
    p.add_argument("--mlvc-qp", default=_csv_ints(he.get("mlvc_qp"), "0,21,42,63"))
    p.add_argument("--mpp-qp", default=_csv_ints(he.get("mpp_qp"), "22,28,34,40,44,51"))
    p.add_argument("--plot-only", action="store_true")
    p.add_argument("--keep-csv", action="store_true", help="追加已有 rd_data.csv，不覆盖")
    p.add_argument("--rkvc-bin", type=Path, default=None, help="rkvc_transcode 路径")
    p.add_argument("--mlvc-platform", default="rk3576")
    p.add_argument("--mlvc-width", type=int, default=MLVC_DEFAULT_W)
    p.add_argument("--mlvc-height", type=int, default=MLVC_DEFAULT_H)
    p.add_argument(
        "--mlvc-model-dir",
        type=Path,
        default=Path(he.get("mlvc_model_dir") or (ROOT / "models" / "mlvc")),
        help="MLVC RKNN + PMF bundle 目录",
    )
    p.add_argument(
        "--mlvc-s-model-dir",
        type=Path,
        default=Path(he.get("mlvc_s_model_dir") or (ROOT / "models" / "mlvc-s")),
        help="MLVC-S RKNN + PMF 目录",
    )
    return p


def parse_int_list(text: str) -> list[int]:
    return [int(x.strip()) for x in text.split(",") if x.strip()]


def main() -> int:
    args = build_parser().parse_args()
    global SEQS
    he = _hevc_e_cfg()
    if he.get("sequences"):
        SEQS = tuple(he["sequences"])
    args.mlvc_qp = parse_int_list(args.mlvc_qp)
    args.mpp_qp = parse_int_list(args.mpp_qp)
    codecs = [c.strip() for c in args.codecs.split(",") if c.strip()]
    if any(c in ("mlvc", "mlvc-s") for c in codecs) and (
        args.width != args.mlvc_width or args.height != args.mlvc_height
    ):
        raise SystemExit(
            "HEVC-E 的所有 codec 必须使用同一评价分辨率；"
            f"当前 MPP={args.width}x{args.height}，"
            f"MLVC={args.mlvc_width}x{args.mlvc_height}"
        )
    out_dir = args.out_dir
    work = out_dir / "work"
    csv_path = out_dir / "rd_data.csv"
    png = out_dir / "hevc_e_msssim.png"

    if args.plot_only:
        rows = load_csv(csv_path)
        if not rows:
            raise SystemExit(f"无 CSV: {csv_path}")
        dims = {(int(r["width"]), int(r["height"])) for r in rows}
        if len(dims) != 1:
            raise SystemExit(f"CSV 混有不同评价分辨率，不能绘制同一 RD 曲线: {sorted(dims)}")
        w, h = next(iter(dims))
        plot_msssim_cbr(
            rows,
            png,
            title=f"HEVC Class E {w}x{h} MS-SSIM: MLVC / MLVC-S / MPP H.264 / MPP H.265",
            footnote=(
                f"本机 RK3576 · HEVC-E（FourPeople / Johnny / KristenAndSara）均值 · "
                f"{w}×{h} · {rows[0]['frames']} 帧 · GOP=60（MPP CQP）· "
                "无信道编码 · 纵轴 MS-SSIM (dB) = -10 log10(1-d)，Y 通道 5 尺度 · 横轴按像素码率对齐"
            ),
            width=w,
            height=h,
        )
        return 0

    if args.keep_csv:
        existing = load_csv(csv_path)
        dims = {(int(r["width"]), int(r["height"])) for r in existing}
        expected = (args.width, args.height)
        if len(dims) > 1 or (dims and dims != {expected}):
            raise SystemExit(
                "--keep-csv 的已有数据与本次评价分辨率不一致；"
                f"已有={sorted(dims)}，本次={expected}"
            )

    clips = prepare_clips(args.src_dir, work / "clips", args.width, args.height, args.frames)

    if csv_path.is_file() and not args.keep_csv:
        csv_path.unlink()

    all_rows: list[dict] = []
    if args.keep_csv:
        all_rows.extend(load_csv(csv_path))
    for codec in codecs:
        if codec in ("h264", "h265"):
            rows = run_mpp(codec, args, clips, work)
        elif codec in ("mlvc", "mlvc-s"):
            rows = run_mlvc(codec, args, work / "clips", work)
        else:
            raise SystemExit(f"未知 codec: {codec}")
        append_csv(csv_path, rows)
        all_rows.extend(rows)

    has_mlvc = any(r.get("codec", "").startswith("mlvc") for r in all_rows)
    footnote = (
        f"本机 RK3576 · HEVC-E（FourPeople / Johnny / KristenAndSara）均值 · "
        f"统一评价分辨率 {args.width}×{args.height}"
        + (" · MLVC/MLVC-S RKNN" if has_mlvc else "")
        + f" · {args.frames} 帧 · GOP=60（MPP CQP）· 无信道编码 · "
        "纵轴 MS-SSIM (dB) = -10 log10(1-d)，Y 通道 5 尺度 · 横轴按像素码率对齐 · "
        "in-tree ffmpeg-rockchip + rkvc_transcode"
    )
    plot_msssim_cbr(
        all_rows,
        png,
        title=f"HEVC Class E MS-SSIM: MLVC / MLVC-S / MPP H.264 / MPP H.265",
        footnote=footnote,
        width=args.width,
        height=args.height,
    )
    print(f"[done] {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
