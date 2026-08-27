#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Verify an RKVC Phase-RLFN bundle before portable packaging."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Sequence


class BundleError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify(bundle: Path) -> dict[str, Any]:
    manifest_path = bundle / "sr_export_manifest.json"
    if not manifest_path.is_file():
        raise BundleError(f"缺少 manifest: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BundleError(f"manifest 无法读取: {exc}") from exc
    if manifest.get("schema") != 1:
        raise BundleError(f"不支持的 schema: {manifest.get('schema')!r}")
    model = manifest.get("model", {})
    if not isinstance(model, dict):
        raise BundleError("manifest.model 必须是对象")
    if model.get("codec_context") is not False:
        raise BundleError("只允许单输入 fallback core (codec_context=false)")
    input_desc = model.get("input")
    output_desc = model.get("output")
    input_shape = input_desc.get("shape") if isinstance(input_desc, dict) else None
    output_shape = output_desc.get("shape") if isinstance(output_desc, dict) else None
    if not isinstance(input_shape, list) or len(input_shape) != 4 or \
       not isinstance(output_shape, list) or len(output_shape) != 4 or \
       input_shape[1] != 12 or output_shape[1] != 108 or \
       input_shape[2:] != output_shape[2:]:
        raise BundleError("模型不是 RKVC 要求的 12→108 phase 契约")

    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict):
        raise BundleError("manifest.artifacts 必须是对象")
    required = {
        "phase_rlfn_sr_x3.onnx",
        "phase_rlfn_sr_x3.rknn",
        "LICENSE.rknn-super-resolution-MIT",
        "SOURCE.md",
    }
    missing_entries = required - artifacts.keys()
    if missing_entries:
        raise BundleError(f"manifest 缺少产物: {', '.join(sorted(missing_entries))}")

    for name, expected in artifacts.items():
        if Path(name).name != name:
            raise BundleError(f"非法 artifact 路径: {name}")
        if not isinstance(expected, dict):
            raise BundleError(f"artifact 元数据必须是对象: {name}")
        path = bundle / name
        if not path.is_file():
            raise BundleError(f"artifact 不存在: {path}")
        if path.stat().st_size != expected.get("bytes"):
            raise BundleError(f"artifact 大小不匹配: {name}")
        digest = sha256_file(path)
        if digest != expected.get("sha256"):
            raise BundleError(f"artifact SHA-256 不匹配: {name}")

    # 避免一次 --encrypt/--no-encrypt 或不同 checkpoint 的残留文件被 cp -a
    # 静默带入 portable 包，却不受当前 manifest 约束。
    declared = set(artifacts)
    for path in bundle.glob("phase_rlfn_sr_x3*"):
        if path.is_file() and path.name not in declared:
            raise BundleError(f"存在 manifest 未登记的陈旧模型产物: {path.name}")
    return manifest


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = verify(args.bundle)
    except BundleError as exc:
        parser.exit(1, f"错误: {exc}\n")
    print(
        f"OK: Phase-RLFN bundle {args.bundle} "
        f"({len(manifest['artifacts'])} artifacts)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
