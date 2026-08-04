#!/usr/bin/env python3
"""绘制端到端编解码性能图（编码/解码速度 fps）。"""

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆

from __future__ import annotations

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

from plot_rd_curve import (
    codec_short_label,
    codecs_from_session_file,
    codecs_from_workdir,
    is_ai_sr,
    is_upscale_group,
    sort_codecs,
    upscale_group_key,
)

# 柱状图按阶段着色（与图例一致）；X 轴区分 codec
STAGE_STYLES: dict[str, dict[str, str | float]] = {
    "encode": {"color": "#4e79a7", "label": "Encode", "alpha": 0.92},
    "decode": {"color": "#59a14f", "label": "Decode", "alpha": 0.85},
    "rga": {"color": "#f28e2b", "label": "RGA upscale", "alpha": 0.85},
    "npu": {"color": "#9467bd", "label": "NPU SR", "alpha": 0.85},
    "write": {"color": "#edc948", "label": "NV12 write", "alpha": 0.85},
    "post": {"color": "#f28e2b", "label": "Post-upscale", "alpha": 0.85},
    "e2e": {"color": "#b07aa1", "label": "E2E", "alpha": 0.80},
}


def _span(stats_list: list[dict], key: str) -> tuple[float, float, float]:
    vals = [s[key] for s in stats_list]
    med = statistics.median(vals)
    return med, min(vals), max(vals)


def merge_upscale_perf(data: dict[str, dict[str, float]]) -> dict[str, dict[str, float]]:
    """将三种插值路线的性能中位数合并为一条，附带 min–max 范围。"""
    groups: dict[str, list[dict[str, float]]] = defaultdict(list)
    out: dict[str, dict[str, float]] = {}

    def _with_identity_range(stats: dict[str, float]) -> dict[str, float]:
        row = dict(stats)
        for key in ("enc_fps", "dec_fps", "rga_fps", "write_fps", "post_fps", "e2e_fps"):
            row[f"{key}_lo"] = stats.get(f"{key}_lo", stats.get(key, 0.0))
            row[f"{key}_hi"] = stats.get(f"{key}_hi", stats.get(key, 0.0))
        return row

    for codec, stats in data.items():
        gk = upscale_group_key(codec)
        if gk:
            groups[gk].append(stats)
        else:
            out[codec] = _with_identity_range(stats)

    for gk, items in groups.items():
        enc_m, enc_lo, enc_hi = _span(items, "enc_fps")
        dec_m, dec_lo, dec_hi = _span(items, "dec_fps")
        rga_m, rga_lo, rga_hi = _span(items, "rga_fps")
        wr_m, wr_lo, wr_hi = _span(items, "write_fps")
        post_m, post_lo, post_hi = _span(items, "post_fps")
        e2e_m, e2e_lo, e2e_hi = _span(items, "e2e_fps")
        out[gk] = {
            "enc_fps": enc_m,
            "enc_fps_lo": enc_lo,
            "enc_fps_hi": enc_hi,
            "dec_fps": dec_m,
            "dec_fps_lo": dec_lo,
            "dec_fps_hi": dec_hi,
            "rga_fps": rga_m,
            "rga_fps_lo": rga_lo,
            "rga_fps_hi": rga_hi,
            "write_fps": wr_m,
            "write_fps_lo": wr_lo,
            "write_fps_hi": wr_hi,
            "post_fps": post_m,
            "post_fps_lo": post_lo,
            "post_fps_hi": post_hi,
            "e2e_fps": e2e_m,
            "e2e_fps_lo": e2e_lo,
            "e2e_fps_hi": e2e_hi,
        }
    return out


def load_perf(path: Path, frames: int) -> dict[str, dict[str, float]]:
    enc_fps: dict[str, list[float]] = defaultdict(list)
    dec_fps: dict[str, list[float]] = defaultdict(list)
    rga_fps: dict[str, list[float]] = defaultdict(list)
    write_fps: dict[str, list[float]] = defaultdict(list)
    post_fps: dict[str, list[float]] = defaultdict(list)
    enc_sec: dict[str, list[float]] = defaultdict(list)
    dec_sec: dict[str, list[float]] = defaultdict(list)
    rga_sec: dict[str, list[float]] = defaultdict(list)
    write_sec: dict[str, list[float]] = defaultdict(list)
    post_sec: dict[str, list[float]] = defaultdict(list)
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            codec = row["codec"]
            enc_s = float(row["encode_sec"])
            dec_s = float(row["decode_sec"])
            rga_s = float(row.get("rga_sec") or 0)
            wr_s = float(row.get("write_sec") or 0)
            post_s = float(row.get("postproc_sec") or 0)
            if post_s <= 0 and (rga_s > 0 or wr_s > 0):
                post_s = rga_s + wr_s
            if rga_s <= 0 and wr_s <= 0 and post_s > 0:
                rga_s = post_s
            enc_sec[codec].append(enc_s)
            dec_sec[codec].append(dec_s)
            rga_sec[codec].append(rga_s)
            write_sec[codec].append(wr_s)
            post_sec[codec].append(post_s)
            if enc_s > 0:
                enc_fps[codec].append(frames / enc_s)
            if dec_s > 0:
                dec_fps[codec].append(frames / dec_s)
            if rga_s > 0:
                rga_fps[codec].append(frames / rga_s)
            if wr_s > 0:
                write_fps[codec].append(frames / wr_s)
            if post_s > 0:
                post_fps[codec].append(frames / post_s)

    out: dict[str, dict[str, float]] = {}
    all_codecs = set(enc_fps) | set(dec_fps) | set(rga_fps) | set(write_fps) | set(post_fps)
    for codec in all_codecs:
        med_enc_s = statistics.median(enc_sec[codec]) if enc_sec[codec] else 0.0
        med_dec_s = statistics.median(dec_sec[codec]) if dec_sec[codec] else 0.0
        med_rga_s = statistics.median(rga_sec[codec]) if rga_sec[codec] else 0.0
        med_wr_s = statistics.median(write_sec[codec]) if write_sec[codec] else 0.0
        med_post_s = statistics.median(post_sec[codec]) if post_sec[codec] else 0.0
        med_enc_fps = statistics.median(enc_fps[codec]) if enc_fps[codec] else 0.0
        med_dec_fps = statistics.median(dec_fps[codec]) if dec_fps[codec] else 0.0
        med_rga_fps = statistics.median(rga_fps[codec]) if rga_fps[codec] else 0.0
        med_wr_fps = statistics.median(write_fps[codec]) if write_fps[codec] else 0.0
        med_post_fps = statistics.median(post_fps[codec]) if post_fps[codec] else 0.0
        total = med_enc_s + med_dec_s + med_rga_s + med_wr_s
        if total <= 0:
            total = med_enc_s + med_dec_s + med_post_s
        out[codec] = {
            "enc_fps": med_enc_fps,
            "dec_fps": med_dec_fps,
            "rga_fps": med_rga_fps,
            "write_fps": med_wr_fps,
            "post_fps": med_post_fps,
            "e2e_fps": frames / total if total > 0 else 0.0,
        }
    return out


def plot_perf(
    data: dict[str, dict[str, float]],
    out_prefix: Path,
    title: str,
    realtime_fps: float,
) -> None:
    data = merge_upscale_perf(data)

    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "axes.unicode_minus": False,
            "figure.dpi": 150,
        }
    )

    codecs = sort_codecs(list(data.keys()))
    labels = [codec_short_label(c) for c in codecs]
    n = len(codecs)
    x = np.arange(n)
    has_rga = any(data[c].get("rga_fps", 0.0) > 0 for c in codecs)
    has_write = any(data[c].get("write_fps", 0.0) > 0 for c in codecs)
    has_post = any(data[c].get("post_fps", 0.0) > 0 for c in codecs)
    has_upscale_range = any(is_upscale_group(c) for c in codecs)
    max_stages = 4 + (1 if has_rga else 0) + (1 if has_write else 0)
    if has_rga and has_post and not has_write:
        max_stages = 4 + (1 if has_rga else 0)
    width = 0.14 if max_stages >= 5 else (0.16 if max_stages >= 4 else 0.22)
    err_kw = dict(capsize=2, capthick=0.8, elinewidth=0.8, ecolor="#444444")

    def _vals(key: str) -> list[float]:
        return [data[c].get(key, 0.0) for c in codecs]

    enc = _vals("enc_fps")
    dec = _vals("dec_fps")
    rga = _vals("rga_fps")
    write = _vals("write_fps")
    post = _vals("post_fps")
    e2e = _vals("e2e_fps")

    def _layout_for(codec_idx: int) -> list[tuple[str, float]]:
        """返回该 codec 要画的 (stage, x_offset) 列表，簇内居中。"""
        codec = codecs[codec_idx]
        stages: list[str] = ["encode", "decode"]
        if has_rga:
            if rga[codec_idx] > 0:
                stages.append("npu" if is_ai_sr(codec) else "rga")
        elif post[codec_idx] > 0:
            stages.append("post")
        if has_write and write[codec_idx] > 0:
            stages.append("write")
        stages.append("e2e")
        offs = _offsets(len(stages))
        return [(stages[j], offs[j]) for j in range(len(stages))]

    def _offsets(count: int) -> list[float]:
        """在 x[i] 两侧对称排布 count 根柱，整体居中于刻度。"""
        if count == 1:
            return [0.0]
        step = width * 1.05
        start = -(count - 1) / 2 * step
        return [start + i * step for i in range(count)]

    stage_vals = {"encode": enc, "decode": dec, "rga": rga, "npu": rga,
                  "write": write, "post": post, "e2e": e2e}

    def _errs(lo_key: str, hi_key: str, med_key: str) -> tuple[np.ndarray, np.ndarray]:
        low = np.array([data[c][med_key] - data[c][lo_key] for c in codecs])
        high = np.array([data[c][hi_key] - data[c][med_key] for c in codecs])
        return low, high

    def _maybe_err(lo_key: str, hi_key: str, med_key: str) -> list[np.ndarray] | None:
        if not has_upscale_range:
            return None
        low, high = _errs(lo_key, hi_key, med_key)
        return [low, high]

    def _maybe_rt_err(lo_key: str, hi_key: str, med_key: str) -> list[np.ndarray] | None:
        if not has_upscale_range:
            return None
        low, high = [], []
        for c in codecs:
            med = data[c][med_key]
            lo = data[c][lo_key]
            hi = data[c][hi_key]
            if med <= 0:
                low.append(0.0)
                high.append(0.0)
                continue
            low.append(max(0.0, realtime_fps / hi - realtime_fps / med))
            high.append(max(0.0, realtime_fps / lo - realtime_fps / med))
        return [np.array(low), np.array(high)]

    def _stage_yerr(stage: str, yerr_fn) -> tuple[float, float] | None:
        lo_key, hi_key, med_key = {
            "encode": ("enc_fps_lo", "enc_fps_hi", "enc_fps"),
            "decode": ("dec_fps_lo", "dec_fps_hi", "dec_fps"),
            "rga": ("rga_fps_lo", "rga_fps_hi", "rga_fps"),
            "npu": ("rga_fps_lo", "rga_fps_hi", "rga_fps"),
            "write": ("write_fps_lo", "write_fps_hi", "write_fps"),
            "post": ("post_fps_lo", "post_fps_hi", "post_fps"),
            "e2e": ("e2e_fps_lo", "e2e_fps_hi", "e2e_fps"),
        }[stage]
        raw = yerr_fn(lo_key, hi_key, med_key)
        if raw is None:
            return None
        return raw  # caller indexes per codec

    def _draw_grouped_bars(ax, vals_map: dict[str, list[float]], *, yerr_fn) -> None:
        drawn_labels: set[str] = set()
        for i in range(n):
            for stage, xoff in _layout_for(i):
                v = vals_map[stage][i]
                if stage in ("post", "rga", "npu", "write") and v <= 0:
                    continue
                style = STAGE_STYLES[stage]
                label = str(style["label"])
                if label in drawn_labels:
                    label = "_nolegend_"
                else:
                    drawn_labels.add(str(style["label"]))
                ye = None
                if yerr_fn is not None:
                    err = _stage_yerr(stage, yerr_fn)
                    if err is not None:
                        low_arr, high_arr = err
                        ye = [[low_arr[i]], [high_arr[i]]]
                ax.bar(
                    i + xoff,
                    v,
                    width=width,
                    label=label,
                    color=str(style["color"]),
                    alpha=float(style["alpha"]),
                    edgecolor="white",
                    linewidth=0.6,
                    yerr=ye,
                    error_kw=err_kw,
                )

    fig_w = max(14, n * 2.8)
    fig, axes = plt.subplots(1, 2, figsize=(fig_w, 5.5))

    ax = axes[0]
    _draw_grouped_bars(
        ax,
        stage_vals,
        yerr_fn=_maybe_err if has_upscale_range else None,
    )
    ax.axhline(realtime_fps, color="#c44e52", linestyle="--", linewidth=1.2,
               label=f"Realtime ({realtime_fps:.0f} fps)")
    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=0, ha="center")
    ax.set_xlim(-0.6, n - 0.4)
    ax.set_ylabel("Throughput (fps, log scale)")
    ax.set_title("Encode / Decode / E2E Speed")
    ax.grid(True, axis="y", linestyle="--", alpha=0.4)
    ax.legend(loc="upper right", fontsize=8, framealpha=0.9)

    for i in range(n):
        for stage, xoff in _layout_for(i):
            v = stage_vals[stage][i]
            if stage in ("post", "rga", "write") and v <= 0:
                continue
            ax.text(
                i + xoff,
                v * 1.08,
                f"{v:.1f}",
                ha="center",
                va="bottom",
                fontsize=7,
            )

    ax2 = axes[1]
    rt_vals = {
        "encode": [realtime_fps / v if v > 0 else float("inf") for v in enc],
        "decode": [realtime_fps / v if v > 0 else float("inf") for v in dec],
        "rga": [realtime_fps / v if v > 0 else 0.0 for v in rga],
        "npu": [realtime_fps / v if v > 0 else 0.0 for v in rga],
        "write": [realtime_fps / v if v > 0 else 0.0 for v in write],
        "post": [realtime_fps / v if v > 0 else 0.0 for v in post],
        "e2e": [realtime_fps / v if v > 0 else float("inf") for v in e2e],
    }
    _draw_grouped_bars(
        ax2,
        rt_vals,
        yerr_fn=_maybe_rt_err if has_upscale_range else None,
    )
    ax2.axhline(1.0, color="#c44e52", linestyle="--", linewidth=1.2, label="Realtime (=1×)")
    ax2.set_yscale("log")
    ax2.set_xticks(x)
    ax2.set_xticklabels(labels, rotation=0, ha="center")
    ax2.set_xlim(-0.6, n - 0.4)
    ax2.set_ylabel("Slower than realtime (×, log scale)")
    ax2.set_title("Realtime Factor (>1 = slower than realtime)")
    ax2.grid(True, axis="y", linestyle="--", alpha=0.4)
    ax2.legend(loc="upper left", fontsize=8, framealpha=0.9)

    fig.suptitle(title, fontsize=12, fontweight="bold", y=1.02)
    fig.tight_layout()
    png = out_prefix.with_suffix(".png")
    pdf = out_prefix.with_suffix(".pdf")
    fig.savefig(png, bbox_inches="tight", pad_inches=0.25)
    fig.savefig(pdf, bbox_inches="tight", pad_inches=0.25)
    print(f"Saved: {png}")
    print(f"Saved: {pdf}")
    plt.close(fig)


def main() -> None:
    bench_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="绘制编解码性能图")
    parser.add_argument("--csv", type=Path, default=bench_root / "results" / "rd_data.csv")
    parser.add_argument("--out", type=Path, default=bench_root / "results" / "perf_e2e")
    parser.add_argument("--frames", type=int, default=62, help="测试片段帧数")
    parser.add_argument("--realtime-fps", type=float, default=30.0)
    parser.add_argument(
        "--title",
        default="E2E Performance (RK3588, median over rate points)",
    )
    parser.add_argument(
        "--session-codecs",
        type=Path,
        default=None,
        help="session.codecs 文件路径",
    )
    parser.add_argument(
        "--filter-workdir",
        type=Path,
        default=None,
        help="（已弃用）仅绘制 workdir/results_*.csv 中的 codec",
    )
    args = parser.parse_args()

    if not args.csv.exists():
        raise SystemExit(f"找不到数据文件: {args.csv}")

    data = load_perf(args.csv, args.frames)
    session_path = args.session_codecs
    if session_path is None:
        auto = args.csv.parent / "session.codecs"
        if auto.is_file():
            session_path = auto
    include = codecs_from_session_file(session_path) if session_path else set()
    if not include and args.filter_workdir and args.filter_workdir.is_dir():
        include = codecs_from_workdir(args.filter_workdir)
    if include:
        data = {k: v for k, v in data.items() if k in include}
    if not data:
        raise SystemExit("CSV 无有效性能数据")
    plot_perf(data, args.out, args.title, args.realtime_fps)


if __name__ == "__main__":
    main()
