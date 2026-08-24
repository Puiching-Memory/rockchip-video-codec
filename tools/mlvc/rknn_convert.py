# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""调用 rknn-toolkit2 把 ONNX 转成 ``.rknn``（FP16，无图像 mean/std）。"""

from __future__ import annotations

import platform
from pathlib import Path
from typing import Any, Sequence, Union

PathLike = Union[str, Path]

DEFAULT_PLATFORMS = ("rk3588", "rv1126b", "rk3576", "rk3568", "rk3566")


class RknnConvertError(RuntimeError):
    """RKNN 转换失败。"""


def has_local_rknn() -> bool:
    try:
        from rknn.api import RKNN  # type: ignore  # noqa: F401
    except ImportError:
        return False
    return True


def require_rknn() -> Any:
    try:
        from rknn.api import RKNN  # type: ignore
    except ImportError as exc:
        machine = platform.machine().lower()
        hint = (
            "需要 rknn-toolkit2（from rknn.api import RKNN）。\n"
            "安装：在仓库根目录运行 uv sync（pyproject.toml 固定版本）\n"
            "运行：.venv/bin/python tools/mlvc/export_rknn.py ...\n"
            "PyPI 有 x86_64 / aarch64 manylinux wheel（Python 3.12）。"
        )
        if "aarch64" in machine or "arm" in machine:
            hint += (
                "\n当前是 ARM：请用 CPython 3.12（见仓库根目录 .python-version），"
                "再在仓库根目录执行 uv sync。"
            )
        raise RknnConvertError(hint) from exc
    return RKNN


def convert_onnx_to_rknn(
    onnx_path: PathLike,
    output_path: PathLike,
    *,
    target: str = "rk3588",
    verbose: bool = False,
    optimization_level: int = 3,
    input_names: Sequence[str] | None = None,
    input_size_list: Sequence[Sequence[int]] | None = None,
) -> Path:
    RKNN = require_rknn()
    src = Path(onnx_path)
    dst = Path(output_path)
    if not src.is_file():
        raise RknnConvertError(f"ONNX 不存在: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=verbose)
    try:
        cfg_kw: dict[str, Any] = {
            "target_platform": target,
            "optimization_level": optimization_level,
            "float_dtype": "float16",
        }
        try:
            ret = rknn.config(**cfg_kw)
        except TypeError:
            cfg_kw.pop("float_dtype", None)
            try:
                ret = rknn.config(**cfg_kw)
            except TypeError:
                cfg_kw.pop("optimization_level", None)
                ret = rknn.config(**cfg_kw)
        if ret not in (None, 0):
            raise RknnConvertError(f"rknn.config 失败: {ret} ({src.name})")

        load_kw: dict[str, Any] = {"model": str(src)}
        if input_names:
            load_kw["inputs"] = list(input_names)
        if input_size_list:
            load_kw["input_size_list"] = [list(s) for s in input_size_list]
        try:
            ret = rknn.load_onnx(**load_kw)
        except TypeError:
            load_kw.pop("inputs", None)
            load_kw.pop("input_size_list", None)
            ret = rknn.load_onnx(**load_kw)
        if ret != 0:
            raise RknnConvertError(f"rknn.load_onnx 失败: {ret} ({src.name})")

        ret = rknn.build(do_quantization=False)
        if ret != 0:
            raise RknnConvertError(f"rknn.build 失败: {ret} ({src.name})")

        ret = rknn.export_rknn(str(dst))
        if ret != 0:
            raise RknnConvertError(f"rknn.export_rknn 失败: {ret} ({dst})")
    finally:
        rknn.release()

    if not dst.is_file():
        raise RknnConvertError(f"未生成 RKNN 文件: {dst}")
    return dst
