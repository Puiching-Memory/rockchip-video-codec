# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""QPT1：QP 条件表二进制（宿主侧查表，喂 qp-dynamic 模型的 q_*_row 输入）。

布局（小端）：
    固定头（8B）：magic "QPT1" + table_count(u32)
    每表：name_len(u32) + name(UTF-8 无 NUL) + rows(u32) + cols(u32)
          + rows*cols 个 fp16（行主序）
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Sequence

MAGIC = b"QPT1"
MAX_NAME = 32


class QptabError(ValueError):
    pass


def write_qptab(tables: Sequence, path: Path) -> int:
    """tables: onnx_rewrite.QpTable 序列（需有 name/rows/cols/data_fp16）。"""
    out = bytearray()
    out += MAGIC
    out += struct.pack("<I", len(tables))
    for t in tables:
        name = t.name.encode("utf-8")
        if not name or len(name) > MAX_NAME:
            raise QptabError(f"表名无效: {t.name!r}")
        if len(t.data_fp16) != t.rows * t.cols * 2:
            raise QptabError(
                f"表 {t.name} 数据长度 {len(t.data_fp16)} != {t.rows}×{t.cols}×2"
            )
        out += struct.pack("<I", len(name))
        out += name
        out += struct.pack("<II", t.rows, t.cols)
        out += t.data_fp16
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    return len(out)
