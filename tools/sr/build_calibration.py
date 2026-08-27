#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build single-input Phase-RLFN RKNN calibration tensors from LR images."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Sequence

import numpy as np


def rgb_to_ycbcr(rgb: np.ndarray) -> np.ndarray:
    values = rgb.astype(np.float32)
    r, g, b = values[..., 0], values[..., 1], values[..., 2]
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    cb = 0.5 * (b - y) / (1.0 - 0.0722) + 127.5
    cr = 0.5 * (r - y) / (1.0 - 0.2126) + 127.5
    return np.rint(np.stack((y, cb, cr), axis=-1).clip(0, 255)).astype(np.uint8)


def pixel_unshuffle_hwc(image: np.ndarray, factor: int = 2) -> np.ndarray:
    h, w, channels = image.shape
    if h % factor or w % factor:
        raise ValueError("image dimensions must be divisible by factor")
    nchw = np.transpose(image, (2, 0, 1))[None]
    packed = nchw.reshape(1, channels, h // factor, factor, w // factor, factor)
    return packed.transpose(0, 1, 3, 5, 2, 4).reshape(
        1, channels * factor * factor, h // factor, w // factor
    )


def simulate_runtime_nv12(ycbcr: np.ndarray) -> np.ndarray:
    """Apply the runtime's 4:2:0 storage and bilinear chroma expansion."""
    height, width, channels = ycbcr.shape
    if channels != 3 or (height & 1) or (width & 1):
        raise ValueError("YCbCr image must be HWC with even dimensions")
    chroma = np.rint(
        ycbcr[..., 1:3].reshape(height // 2, 2, width // 2, 2, 2)
        .mean(axis=(1, 3))
    ).astype(np.uint16)
    right = np.concatenate((chroma[:, 1:], chroma[:, -1:]), axis=1)
    bottom = np.concatenate((chroma[1:], chroma[-1:]), axis=0)
    diagonal = np.concatenate((bottom[:, 1:], bottom[:, -1:]), axis=1)
    expanded = np.empty((height, width, 2), dtype=np.uint8)
    expanded[0::2, 0::2] = chroma
    expanded[0::2, 1::2] = ((chroma + right + 1) // 2).astype(np.uint8)
    expanded[1::2, 0::2] = ((chroma + bottom + 1) // 2).astype(np.uint8)
    expanded[1::2, 1::2] = (
        (chroma + right + bottom + diagonal + 2) // 4
    ).astype(np.uint8)
    return np.concatenate((ycbcr[..., :1], expanded), axis=-1)


def build(input_dir: Path, output_dir: Path, output_list: Path,
          *, width: int, height: int, limit: int) -> int:
    try:
        import cv2
    except ImportError as exc:
        raise ValueError("读取校准图片需要项目 uv 环境中的 opencv-python") from exc
    paths = sorted(
        path for path in input_dir.rglob("*")
        if path.suffix.lower() in {".png", ".jpg", ".jpeg", ".bmp", ".webp"}
    )[:limit]
    if not paths:
        raise ValueError(f"未找到校准图片: {input_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    output_list.parent.mkdir(parents=True, exist_ok=True)
    tensors: list[Path] = []
    for index, path in enumerate(paths):
        bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        rgb = cv2.cvtColor(cv2.resize(bgr, (width, height),
                                      interpolation=cv2.INTER_CUBIC), cv2.COLOR_BGR2RGB)
        tensor = pixel_unshuffle_hwc(simulate_runtime_nv12(rgb_to_ycbcr(rgb)))
        target = (output_dir / f"phase_{index:04d}.npy").resolve()
        np.save(target, tensor)
        tensors.append(target)
    if not tensors:
        raise ValueError("所有校准图片均读取失败")
    output_list.write_text("".join(f"{path}\n" for path in tensors), encoding="utf-8")
    return len(tensors)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_dir", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(".build/sr-calibration/tensors"))
    parser.add_argument("--output-list", type=Path,
                        default=Path(".build/sr-calibration/calibration.txt"))
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--limit", type=int, default=100)
    args = parser.parse_args(argv)
    count = build(args.input_dir, args.output_dir, args.output_list,
                  width=args.width, height=args.height, limit=args.limit)
    print(f"wrote {count} calibration tensors to {args.output_list}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
