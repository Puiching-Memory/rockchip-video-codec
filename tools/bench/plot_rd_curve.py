#!/usr/bin/env python3
"""绘制端到端编解码 RD 曲线（按 actual_kbps 自适应横轴）。"""

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆

from __future__ import annotations

import argparse
import csv
import re
import statistics
from collections import defaultdict
from pathlib import Path

import math
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, FixedLocator, NullFormatter, ScalarFormatter

# post-upscale 基线 codec 单一来源（shell 侧为 POST_UPSCALE_BASE_LIST）
UPSCALE_BASES = ("h264", "h265", "svt-av1", "svt-av1-hq")
_BASE_RE = "|".join(UPSCALE_BASES)

UPSCALE_CODEC_RE = re.compile(
    rf"^(?P<base>{_BASE_RE})\+up(?P<scale>\d+)x-(?P<algo>[A-Za-z0-9_]+)$"
)

# RGA 插值算法（合并为均值带）；AI 超分单独成线
RGA_ALGOS = frozenset({"nearest", "bilinear", "bicubic"})
# mobileone：历史 CSV 别名，等同 rkvc_sr
AI_SR_ALGOS = frozenset({"rkvc_sr", "mobileone"})

UPSCALE_GROUP_RE = re.compile(
    rf"^(?P<base>{_BASE_RE})\+up(?P<scale>\d+)x$"
)

CODEC_LABELS = {
    "h264": "H.264",
    "h265": "H.265",
    "svt-av1": "SVT-AV1",
    "svt-av1-hq": "SVT-AV1 HQ (non-realtime)",
    "svt-av1+superres": "SVT-AV1 + superres (AV1 built-in)",
    "rkvc-realtime": "rkvc realtime (H.264)",
    "rkvc-balanced": "rkvc balanced (HEVC)",
    "rkvc-quality": "rkvc quality (AV1)",
    "rkvc-offline": "rkvc offline (AV1 HQ)",
    "rkvc-neural": "rkvc neural (MLVC)",
    "rkvc": "rkvc (Session)",
}

# 按 codec 技术家族配色：同族相近色相，基线/RGA/AI 用深→浅区分
FAMILY_PALETTES: dict[str, dict[str, str]] = {
    "h264": {
        "base": "#1B4F8A",  # 深蓝
        "rga": "#4A90D9",   # 中蓝
        "ai": "#8CB8E8",    # 浅蓝
    },
    "h265": {
        "base": "#B45309",  # 深橙
        "rga": "#E8923A",   # 中橙
        "ai": "#F5C07A",    # 浅橙
    },
    "svt-av1": {
        "base": "#1B7D3A",  # 深绿
        "rga": "#4DAF6C",   # 中绿
        "ai": "#8FD4A8",    # 浅绿
    },
    "svt-av1-hq": {
        "base": "#0F5C2E",  # 更深绿（HQ）
        "rga": "#2F8F55",
        "ai": "#6FBF8A",
    },
}

CODEC_COLORS = {
    "h264": FAMILY_PALETTES["h264"]["base"],
    "h265": FAMILY_PALETTES["h265"]["base"],
    "svt-av1": FAMILY_PALETTES["svt-av1"]["base"],
    "svt-av1-hq": "#0F5C2E",
    "svt-av1+superres": "#14532D",
    "rkvc-realtime": FAMILY_PALETTES["h264"]["rga"],
    "rkvc-balanced": FAMILY_PALETTES["h265"]["rga"],
    "rkvc-quality": FAMILY_PALETTES["svt-av1"]["rga"],
    "rkvc-offline": FAMILY_PALETTES["svt-av1"]["ai"],
    "rkvc-neural": "#e377c2",
    "rkvc": "#9467bd",
}

UPSCALE_BASE_LABELS = {base: CODEC_LABELS[base].split(" (")[0] for base in UPSCALE_BASES}

UPSCALE_ALGO_COLORS = {
    base: {
        "nearest": FAMILY_PALETTES[base]["rga"],
        "bilinear": FAMILY_PALETTES[base]["rga"],
        "bicubic": FAMILY_PALETTES[base]["rga"],
        "rkvc_sr": FAMILY_PALETTES[base]["ai"],
        "mobileone": FAMILY_PALETTES[base]["ai"],
    }
    for base in FAMILY_PALETTES
}

UPSCALE_ALGO_MARKERS = {
    "nearest": "x",
    "bilinear": "d",
    "bicubic": "p",
    "rkvc_sr": "*",
    "mobileone": "*",
}

# 合并后的 post-upscale RGA 曲线颜色（家族中色）
UPSCALE_GROUP_COLORS = {base: pal["rga"] for base, pal in FAMILY_PALETTES.items()}

CODEC_MARKERS = {
    "h264": "o",
    "h265": "s",
    "svt-av1": "^",
    "svt-av1-hq": "*",
    "svt-av1+superres": "h",
    "rkvc-realtime": "D",
    "rkvc-balanced": "v",
    "rkvc-quality": "P",
    "rkvc-offline": "X",
    "rkvc-neural": "h",
    "rkvc": "v",
}

SIMPLE_BASELINE_CODECS = ("h264", "h265", "svt-av1")
SIMPLE_MAX_KBPS = 1000.0
SIMPLE_LABELS = {
    "h264": "H.264",
    "h265": "H.265",
    "svt-av1": "RKVC-Quality",
}

# 图例顺序 = CODEC_LABELS 定义顺序（单一来源）
CODEC_ORDER = list(CODEC_LABELS)


def is_superres_variant(codec: str) -> bool:
    return codec == "svt-av1+superres"


def is_ai_sr(codec: str) -> bool:
    m = UPSCALE_CODEC_RE.match(codec)
    return bool(m and m.group("algo") in AI_SR_ALGOS)


def codec_linestyle(codec: str) -> str:
    if is_superres_variant(codec) or is_upscale_group(codec):
        return "--"
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        return "-." if m.group("algo") in AI_SR_ALGOS else "--"
    return "-"


def is_post_upscale(codec: str) -> bool:
    return bool(UPSCALE_CODEC_RE.match(codec))


def is_upscale_group(codec: str) -> bool:
    return bool(UPSCALE_GROUP_RE.match(codec))


def upscale_group_key(codec: str) -> str | None:
    m = UPSCALE_CODEC_RE.match(codec)
    if not m or m.group("algo") not in RGA_ALGOS:
        return None
    return f"{m.group('base')}+up{m.group('scale')}x"


def upscale_group_base(codec: str) -> str | None:
    m = UPSCALE_GROUP_RE.match(codec) or UPSCALE_CODEC_RE.match(codec)
    return m.group("base") if m else None


def upscale_group_scale(codec: str) -> int | None:
    m = UPSCALE_GROUP_RE.match(codec) or UPSCALE_CODEC_RE.match(codec)
    return int(m.group("scale")) if m else None


def upscale_group_label(codec: str) -> str:
    base = upscale_group_base(codec) or codec
    scale = upscale_group_scale(codec) or 0
    name = UPSCALE_BASE_LABELS.get(base, base)
    return f"{name} ↑{scale}× RGA" if scale else name


def upscale_group_color(codec: str) -> str:
    base = upscale_group_base(codec) or codec
    return UPSCALE_GROUP_COLORS.get(base, "#888888")


def codec_label(codec: str) -> str:
    if is_upscale_group(codec):
        return upscale_group_label(codec)
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        base = m.group("base")
        scale = int(m.group("scale"))
        algo = m.group("algo")
        if algo in AI_SR_ALGOS:
            return f"{UPSCALE_BASE_LABELS.get(base, base)} ↑{scale}× AI"
        return f"{UPSCALE_BASE_LABELS.get(base, base)} ↑{scale}× RGA ({algo})"
    return CODEC_LABELS.get(codec, codec)


def codec_short_label(codec: str) -> str:
    """柱状图 X 轴用短标签（图例仍用 codec_label）。"""
    if is_upscale_group(codec):
        base = upscale_group_base(codec) or codec
        scale = upscale_group_scale(codec) or 0
        name = UPSCALE_BASE_LABELS.get(base, base)
        return f"{name}↑{scale}×" if scale else name
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        base = m.group("base")
        scale = int(m.group("scale"))
        name = UPSCALE_BASE_LABELS.get(base, base)
        if m.group("algo") in AI_SR_ALGOS:
            return f"{name}↑{scale}× AI"
        return f"{name}↑{scale}×"
    short = {
        "h264": "H.264",
        "h265": "H.265",
        "svt-av1": "SVT-AV1",
        "svt-av1-hq": "SVT-AV1 HQ",
        "svt-av1+superres": "SVT-AV1+SR",
        "rkvc-realtime": "rkvc RT",
        "rkvc-balanced": "rkvc Bal",
        "rkvc-quality": "rkvc Q",
        "rkvc-offline": "rkvc Off",
        "rkvc-neural": "rkvc Neu",
        "rkvc": "rkvc",
    }
    return short.get(codec, codec)


def codec_color(codec: str) -> str:
    if is_upscale_group(codec):
        return upscale_group_color(codec)
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        base = m.group("base")
        algo = m.group("algo")
        mapped = UPSCALE_ALGO_COLORS.get(base, {}).get(algo)
        if mapped:
            return mapped
        # 未知算法：RGA 族用中色，其余用 AI 浅色，避免落入 matplotlib 默认色环
        pal = FAMILY_PALETTES.get(base)
        if pal:
            return pal["rga"] if algo in RGA_ALGOS else pal["ai"]
        return "#888888"
    return CODEC_COLORS.get(codec, "#555555")


def codec_marker(codec: str) -> str:
    if is_upscale_group(codec):
        return "D"
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        algo = m.group("algo")
        if algo in UPSCALE_ALGO_MARKERS:
            return UPSCALE_ALGO_MARKERS[algo]
        return "*" if algo in AI_SR_ALGOS else "o"
    return CODEC_MARKERS.get(codec, "o")


def codec_linewidth(codec: str) -> float:
    if is_upscale_group(codec):
        return 2.0
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        return 2.0
    return 2.4


def codec_zorder(codec: str) -> int:
    """绘制层级：RGA 最底 → AI → 全分辨率基线 → HQ/rkvc 最上。"""
    if is_upscale_group(codec):
        return 2
    m = UPSCALE_CODEC_RE.match(codec)
    if m:
        return 3 if m.group("algo") in AI_SR_ALGOS else 2
    if codec in ("svt-av1-hq", "rkvc-offline"):
        return 6
    if codec.startswith("rkvc-") or codec == "rkvc":
        return 5
    if codec in ("svt-av1", "svt-av1+superres", "h265", "h264"):
        return 4
    return 4


def plot_style_kwargs(codec: str, color: str, label: str) -> dict:
    return dict(
        label=label,
        color=color,
        marker=codec_marker(codec),
        linewidth=codec_linewidth(codec),
        linestyle=codec_linestyle(codec),
        markersize=7 if not is_ai_sr(codec) else 8,
        markeredgecolor="white",
        markeredgewidth=0.4,
        clip_on=True,
        zorder=codec_zorder(codec),
    )


def sort_codecs(codecs: list[str]) -> list[str]:
    groups_present = sorted(
        {c for c in codecs if is_upscale_group(c)}
        | {gk for c in codecs if (gk := upscale_group_key(c))}
    )
    ai_upscales = sorted(
        c for c in codecs if is_post_upscale(c) and upscale_group_key(c) is None
    )
    baselines = [
        c for c in codecs
        if not is_upscale_group(c) and not is_post_upscale(c)
    ]

    ordered: list[str] = []
    for c in CODEC_ORDER:
        if c in baselines:
            ordered.append(c)
        for g in groups_present:
            if g.startswith(f"{c}+up"):
                ordered.append(g)
        for ai in ai_upscales:
            if ai.startswith(f"{c}+up"):
                ordered.append(ai)
    for c in baselines + groups_present + ai_upscales:
        if c not in ordered:
            ordered.append(c)
    return ordered


def group_upscale_rd(data: dict[str, list[dict]]) -> dict[str, list[dict]]:
    """将三种插值曲线按 target_kbps 聚合为均值 ± min/max 带。"""
    grouped: dict[str, dict[float, list[dict]]] = defaultdict(lambda: defaultdict(list))
    baselines: dict[str, list[dict]] = {}

    for codec, pts in data.items():
        gk = upscale_group_key(codec)
        if gk:
            for p in pts:
                grouped[gk][p["target_kbps"]].append(p)
        else:
            baselines[codec] = pts

    out = dict(baselines)
    for gk, by_target in grouped.items():
        merged: list[dict] = []
        for target in sorted(by_target):
            rows = by_target[target]
            brs = [r["actual_kbps"] for r in rows]
            psnr = [r["psnr_y"] for r in rows]
            ssim = [r["ssim"] for r in rows]
            merged.append(
                {
                    "target_kbps": target,
                    "actual_kbps": statistics.mean(brs),
                    "actual_kbps_lo": min(brs),
                    "actual_kbps_hi": max(brs),
                    "psnr_y": statistics.mean(psnr),
                    "psnr_y_lo": min(psnr),
                    "psnr_y_hi": max(psnr),
                    "ssim": statistics.mean(ssim),
                    "ssim_lo": min(ssim),
                    "ssim_hi": max(ssim),
                }
            )
        out[gk] = merged
    return out


def filter_rd_outliers(data: dict[str, list[dict]]) -> dict[str, list[dict]]:
    """剔除相对邻点质量骤降的坏点（如解码失败导致的 PSNR 塌陷）。"""
    out: dict[str, list[dict]] = {}
    for codec, pts in data.items():
        if len(pts) < 3:
            out[codec] = pts
            continue
        sorted_pts = sorted(pts, key=lambda p: p["actual_kbps"])
        kept: list[dict] = []
        for i, p in enumerate(sorted_pts):
            neigh_psnr: list[float] = []
            neigh_ssim: list[float] = []
            if i > 0:
                neigh_psnr.append(sorted_pts[i - 1]["psnr_y"])
                neigh_ssim.append(sorted_pts[i - 1]["ssim"])
            if i + 1 < len(sorted_pts):
                neigh_psnr.append(sorted_pts[i + 1]["psnr_y"])
                neigh_ssim.append(sorted_pts[i + 1]["ssim"])
            if not neigh_psnr:
                kept.append(p)
                continue
            med_psnr = statistics.median(neigh_psnr)
            med_ssim = statistics.median(neigh_ssim)
            if p["psnr_y"] < med_psnr - 6.0 and p["ssim"] < med_ssim - 0.08:
                print(
                    f"[warn] 跳过异常点 {codec} @ {p['target_kbps']:.0f}kbps: "
                    f"PSNR-Y {p['psnr_y']:.1f} (邻点 ~{med_psnr:.1f})"
                )
                continue
            kept.append(p)
        out[codec] = kept
    return out


def load_csv(path: Path, max_kbps: float | None = None) -> dict[str, list[dict]]:
    data: dict[str, list[dict]] = defaultdict(list)
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            codec = row["codec"]
            data[codec].append(
                {
                    "target_kbps": float(row["target_kbps"]),
                    "actual_kbps": float(row["actual_kbps"]),
                    "psnr_y": float(row["psnr_y"]),
                    "psnr_avg": float(row["psnr_avg"]),
                    "ssim": float(row["ssim"]),
                }
            )
    if max_kbps is None:
        all_targets = [p["target_kbps"] for pts in data.values() for p in pts]
        if all_targets:
            # 按 target 扫点上限裁剪 actual，允许适度超发（CQP/CRF 未命中 target）
            max_kbps = max(all_targets) * 3.0
    if max_kbps is not None:
        max_target = max(
            (p["target_kbps"] for pts in data.values() for p in pts),
            default=max_kbps,
        )
        for codec in data:
            data[codec] = [
                p
                for p in data[codec]
                if p["actual_kbps"] <= max_kbps or p["target_kbps"] <= max_target
            ]
    for codec in data:
        data[codec].sort(key=lambda x: x["actual_kbps"])
    return filter_rd_outliers(data)


def _log_kbps_formatter(x, _pos) -> str:
    if x < 10:
        return f"{x:g}"
    return f"{int(round(x))}"


def codecs_from_workdir(workdir: Path) -> set[str]:
    """读取本次 bench 分片 CSV 中的 codec 列表（兼容旧流程）。"""
    found: set[str] = set()
    for partial in sorted(workdir.glob("results_*.csv")):
        with partial.open(newline="") as f:
            for row in csv.DictReader(f):
                if row.get("codec"):
                    found.add(row["codec"])
    return found


def codecs_from_session_file(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    return {line.strip() for line in path.read_text().splitlines() if line.strip()}


def filter_data_codecs(
    data: dict[str, list[dict]], include: set[str] | None
) -> dict[str, list[dict]]:
    if not include:
        return data
    return {k: v for k, v in data.items() if k in include}


def _log_axis_ticks(lo: float, hi: float) -> list[float]:
    """在 [lo, hi] 内选取对数轴刻度（1-2-5 系列）。"""
    if lo <= 0 or hi <= lo:
        return [lo, hi]
    exp_lo = int(math.floor(math.log10(lo)))
    exp_hi = int(math.ceil(math.log10(hi)))
    ticks: list[float] = []
    for exp in range(exp_lo, exp_hi + 1):
        for base in (1, 2, 5):
            v = base * (10**exp)
            if lo * 0.92 <= v <= hi * 1.08:
                ticks.append(float(v))
    return sorted(set(ticks))


def plot_rd(
    data: dict[str, list[dict]],
    out_prefix: Path,
    title: str,
    *,
    xscale: str = "log",
    label_overrides: dict[str, str] | None = None,
    figsize: tuple[float, float] = (12, 5),
    legend_ncol: int | None = None,
) -> None:
    data = group_upscale_rd(data)

    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "axes.unicode_minus": False,
            "figure.dpi": 150,
        }
    )

    fig, axes = plt.subplots(1, 2, figsize=figsize)
    ax_psnr, ax_ssim = axes

    all_br: list[float] = []
    all_psnr: list[float] = []
    all_ssim: list[float] = []

    for codec in sort_codecs(list(data.keys())):
        if codec not in data:
            continue
        pts = data[codec]
        br = [p["actual_kbps"] for p in pts]
        psnr = [p["psnr_y"] for p in pts]
        ssim = [p["ssim"] for p in pts]
        all_br.extend(br)
        all_psnr.extend(psnr)
        all_ssim.extend(ssim)
        if is_upscale_group(codec):
            all_psnr.extend(p["psnr_y_lo"] for p in pts)
            all_psnr.extend(p["psnr_y_hi"] for p in pts)
            all_ssim.extend(p["ssim_lo"] for p in pts)
            all_ssim.extend(p["ssim_hi"] for p in pts)

        color = upscale_group_color(codec) if is_upscale_group(codec) else codec_color(codec)
        label = (label_overrides or {}).get(codec, codec_label(codec))
        z = codec_zorder(codec)
        if is_upscale_group(codec):
            psnr_lo = [p["psnr_y_lo"] for p in pts]
            psnr_hi = [p["psnr_y_hi"] for p in pts]
            ssim_lo = [p["ssim_lo"] for p in pts]
            ssim_hi = [p["ssim_hi"] for p in pts]
            kw = plot_style_kwargs(codec, color, label)
            ax_psnr.plot(br, psnr, **kw)
            ax_ssim.plot(br, ssim, **kw)
        else:
            kw = plot_style_kwargs(codec, color, label)
            ax_psnr.plot(br, psnr, **kw)
            ax_ssim.plot(br, ssim, **kw)

    if not all_br:
        raise SystemExit("无有效码率点可绘图")

    br_min, br_max = min(all_br), max(all_br)
    psnr_min, psnr_max = min(all_psnr), max(all_psnr)
    ssim_min, ssim_max = min(all_ssim), max(all_ssim)
    br_pad = max(50, (br_max - br_min) * 0.08)
    psnr_pad = max(0.5, (psnr_max - psnr_min) * 0.12)
    ssim_pad = max(0.01, (ssim_max - ssim_min) * 0.12)

    if xscale == "log":
        x_left = max(br_min * 0.88, 8.0)
        x_right = max(br_max * 1.12, x_left * 1.5)
        ticks = _log_axis_ticks(x_left, x_right)
        for ax in (ax_psnr, ax_ssim):
            ax.set_xscale("log")
            ax.set_xlim(x_left, x_right)
            # 1-2-5 刻度；避免 LogLocator(base=10) 在 200–7000 区间只剩一个「1000」
            if ticks:
                ax.xaxis.set_major_locator(FixedLocator(ticks))
            ax.xaxis.set_minor_formatter(NullFormatter())
            ax.xaxis.set_major_formatter(FuncFormatter(_log_kbps_formatter))
            ax.xaxis.set_tick_params(which="minor", size=0)
            for label in ax.get_xticklabels():
                label.set_rotation(0)
                label.set_ha("center")
    else:
        ax_psnr.set_xlim(br_min - br_pad, br_max + br_pad)
        ax_ssim.set_xlim(br_min - br_pad, br_max + br_pad)
        for ax in (ax_psnr, ax_ssim):
            ax.xaxis.set_major_formatter(ScalarFormatter(useOffset=False))
            ax.ticklabel_format(axis="x", style="plain", useOffset=False)

    ax_psnr.set_ylim(psnr_min - psnr_pad, psnr_max + psnr_pad)
    ax_ssim.set_ylim(ssim_min - ssim_pad, min(1.0, ssim_max + ssim_pad))

    for ax, ylabel in ((ax_psnr, "PSNR-Y (dB)"), (ax_ssim, "SSIM")):
        ax.set_xlabel("Actual Bitrate (kbps)")
        ax.set_ylabel(ylabel)
        ax.grid(True, linestyle="--", alpha=0.45)

    handles, labels = ax_psnr.get_legend_handles_labels()
    n = max(len(labels), 1)
    ncol = legend_ncol if legend_ncol is not None else (3 if n > 6 else 2)
    fig.legend(
        handles,
        labels,
        loc="upper center",
        bbox_to_anchor=(0.5, -0.02),
        ncol=ncol,
        fontsize=8.5,
        framealpha=0.95,
        columnspacing=1.2,
        handletextpad=0.4,
    )

    fig.suptitle(title, fontsize=11, fontweight="bold", y=0.98)
    fig.tight_layout(rect=(0, 0.10, 1, 0.94))
    png = out_prefix.with_suffix(".png")
    pdf = out_prefix.with_suffix(".pdf")
    fig.savefig(png, bbox_inches="tight", pad_inches=0.25)
    fig.savefig(pdf, bbox_inches="tight", pad_inches=0.25)
    print(f"Saved: {png}")
    print(f"Saved: {pdf}")
    plt.close(fig)


def main() -> None:
    bench_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="绘制 RD 曲线")
    parser.add_argument("--csv", type=Path, default=bench_root / "results" / "rd_data.csv")
    parser.add_argument("--out", type=Path, default=bench_root / "results" / "rd_curve_e2e")
    parser.add_argument(
        "--title",
        default="E2E RD Curve (RK3588, baselines + rkvc realtime/balanced/quality/offline/neural)",
    )
    parser.add_argument(
        "--xscale",
        choices=("log", "linear"),
        default="log",
        help="横轴刻度：log 便于展示低码率上采样优势区（默认 log）",
    )
    parser.add_argument(
        "--max-kbps",
        type=float,
        default=None,
        help="过滤 actual_kbps 上限（默认 max(target_kbps)×3）",
    )
    parser.add_argument(
        "--session-codecs",
        type=Path,
        default=None,
        help="session.codecs 文件路径（与 rd_data.csv 同目录时自动发现）",
    )
    parser.add_argument(
        "--filter-workdir",
        type=Path,
        default=None,
        help="（已弃用）仅绘制 workdir/results_*.csv 中的 codec；请用 session 模式 CSV",
    )
    parser.add_argument(
        "--simple",
        action="store_true",
        help="简化版：仅 H.264 / H.265 / AV1 三条基线",
    )
    args = parser.parse_args()

    if not args.csv.exists():
        raise SystemExit(f"找不到数据文件: {args.csv}")

    data = load_csv(args.csv, max_kbps=args.max_kbps)
    session_path = args.session_codecs
    if session_path is None:
        auto = args.csv.parent / "session.codecs"
        if auto.is_file():
            session_path = auto
    include = codecs_from_session_file(session_path) if session_path else set()
    if not include and args.filter_workdir and args.filter_workdir.is_dir():
        include = codecs_from_workdir(args.filter_workdir)
    if args.simple:
        data = filter_data_codecs(data, set(SIMPLE_BASELINE_CODECS))
        data = {
            codec: [p for p in pts if p["actual_kbps"] <= SIMPLE_MAX_KBPS]
            for codec, pts in data.items()
        }
        data = {codec: pts for codec, pts in data.items() if pts}
    elif include:
        data = filter_data_codecs(data, include)
    if not data:
        raise SystemExit("CSV 无有效数据")
    # 扫点较少且码率跨度不大时用线性横轴，避免对数轴刻度混乱
    max_pts = max(len(v) for v in data.values())
    br_all = [p["actual_kbps"] for pts in data.values() for p in pts]
    xscale = args.xscale
    if xscale == "log" and max_pts <= 6 and br_all and max(br_all) / max(min(br_all), 1) < 20:
        xscale = "linear"
    if args.simple:
        out = args.out if args.out.name != "rd_curve_e2e" else args.out.parent / "rd_curve_e2e_simple"
        title = (
            args.title
            if args.title != parser.get_default("title")
            else "E2E RD (H.264 / H.265 / RKVC-Quality)"
        )
        plot_rd(
            data,
            out,
            title,
            xscale=xscale,
            label_overrides=SIMPLE_LABELS,
            figsize=(10, 4.5),
            legend_ncol=3,
        )
    else:
        plot_rd(
            data,
            args.out,
            args.title,
            xscale=xscale,
        )


if __name__ == "__main__":
    main()
