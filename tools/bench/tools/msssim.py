#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""Y 通道 5 尺度 MS-SSIM，对齐 microsoft/mlvc ``video/src/metrics/msssim.py``。

纵轴 dB：``-10 log10(1 - d)``。CBR = bpp/8。
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np

FILTER_SIZE = 11
FILTER_SIGMA = 1.5
WEIGHT5 = np.array([0.0448, 0.2856, 0.3001, 0.2363, 0.1333], dtype=np.float64)
WEIGHT4 = np.array([0.0517, 0.3295, 0.3462, 0.2726], dtype=np.float64)


def msssim_db(d: float) -> float:
    d = min(max(float(d), 0.0), 1.0 - 1e-12)
    return float(-10.0 * math.log10(1.0 - d))


def cbr_milli(bpp: float) -> float:
    """CBR = bpp/8，再乘 10^3 以便画在 ×10^{-3} 轴上。"""
    return float(bpp) / 8.0 * 1000.0


def _gaussian_1d(size: int = FILTER_SIZE, sigma: float = FILTER_SIGMA) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(x * x) / (2.0 * sigma * sigma))
    k /= k.sum()
    return k


def _conv_sep(img: np.ndarray, k: np.ndarray) -> np.ndarray:
    """对 H×W 图像做可分高斯卷积（先宽后高），valid 模式。"""
    from scipy.ndimage import convolve1d

    tmp = convolve1d(img, k, axis=1, mode="constant", cval=0.0)
    out = convolve1d(tmp, k, axis=0, mode="constant", cval=0.0)
    pad = k.size // 2
    return out[pad:-pad, pad:-pad]


def _ssim_map(x1: np.ndarray, x2: np.ndarray, k: np.ndarray, *, full: bool) -> np.ndarray:
    c1 = (0.01) ** 2
    c2 = (0.03) ** 2
    mu1 = _conv_sep(x1, k)
    mu2 = _conv_sep(x2, k)
    mu1_sq = mu1 * mu1
    mu2_sq = mu2 * mu2
    mu1_mu2 = mu1 * mu2
    sigma1_sq = _conv_sep(x1 * x1, k) - mu1_sq
    sigma2_sq = _conv_sep(x2 * x2, k) - mu2_sq
    sigma12 = _conv_sep(x1 * x2, k) - mu1_mu2
    cs = (2.0 * sigma12 + c2) / (sigma1_sq + sigma2_sq + c2)
    if not full:
        return cs
    return cs * (2.0 * mu1_mu2 + c1) / (mu1_sq + mu2_sq + c1)


def _downsample(x: np.ndarray) -> np.ndarray:
    h, w = x.shape
    if w & 1:
        x = np.concatenate((x, x[:, -1:]), axis=1)
        w += 1
    if h & 1:
        x = np.concatenate((x, x[-1:, :]), axis=0)
        h += 1
    return x.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))


def ms_ssim_y(y1: np.ndarray, y2: np.ndarray) -> float:
    """两帧 Y 平面（float 0..1）的 MS-SSIM 标量。"""
    h, w = y1.shape
    if w >= 16 * FILTER_SIZE and h >= 16 * FILTER_SIZE:
        weight = WEIGHT5
    else:
        if not (w >= 8 * FILTER_SIZE and h >= 8 * FILTER_SIZE):
            raise ValueError(f"Y 平面过小无法算 4/5 尺度 MS-SSIM: {w}x{h}")
        weight = WEIGHT4
    k = _gaussian_1d()
    x1 = y1.astype(np.float64, copy=False)
    x2 = y2.astype(np.float64, copy=False)
    vals = []
    for _ in range(len(weight) - 1):
        vals.append(float(_ssim_map(x1, x2, k, full=False).mean()))
        x1 = _downsample(x1)
        x2 = _downsample(x2)
    vals.append(float(_ssim_map(x1, x2, k, full=True).mean()))
    stacked = np.maximum(np.array(vals, dtype=np.float64), 1.0e-12)
    return float(np.exp(np.sum(weight * np.log(stacked))))


def iter_yuv420_y(path: Path, width: int, height: int, frames: int):
    frame_bytes = width * height * 3 // 2
    y_bytes = width * height
    with path.open("rb") as f:
        for _ in range(frames):
            buf = f.read(frame_bytes)
            if len(buf) < frame_bytes:
                break
            y = np.frombuffer(buf[:y_bytes], dtype=np.uint8).reshape(height, width)
            yield y.astype(np.float64) / 255.0


def mean_ms_ssim_y(
    ref: Path,
    rec: Path,
    width: int,
    height: int,
    frames: int,
) -> float:
    acc = 0.0
    n = 0
    for a, b in zip(
        iter_yuv420_y(ref, width, height, frames),
        iter_yuv420_y(rec, width, height, frames),
    ):
        acc += ms_ssim_y(a, b)
        n += 1
    if n == 0:
        raise RuntimeError(f"无有效帧: {ref} vs {rec}")
    if n != frames:
        print(f"[warn] 只比了 {n}/{frames} 帧 ({ref.name} vs {rec.name})")
    return acc / n
