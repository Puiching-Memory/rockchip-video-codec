# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""Microsoft MLVC PMF JSON ↔ rkvc ``PMF1`` 二进制表。

二进制布局与 ``lib/node_mlvc.c`` ``load_pmf()`` 一致：

    magic "PMF1"
    uint32 nL, nO, nT
    int32  lengths[nL], offsets[nO], table[nT]
    tag=1  gaussian: double scale_min, scale_max; uint32 scale_levels, index_space
    tag=2  bitest:   uint32 qp_num, channels
"""

from __future__ import annotations

import json
import struct
from pathlib import Path
from typing import Any, Mapping, Sequence, Union

PathLike = Union[str, Path]

PMF_MAGIC = b"PMF1"
TAG_GAUSSIAN = 1
TAG_BITEST = 2

_I32 = struct.Struct("<i")
_U32 = struct.Struct("<I")
_F64 = struct.Struct("<d")


class PmfError(ValueError):
    """PMF 文件格式或字段错误。"""


def _as_int_list(name: str, values: Sequence[Any]) -> list[int]:
    out: list[int] = []
    for i, raw in enumerate(values):
        try:
            out.append(int(raw))
        except (TypeError, ValueError) as exc:
            raise PmfError(f"{name}[{i}] 不是整数: {raw!r}") from exc
    return out


def _require(data: Mapping[str, Any], *keys: str) -> None:
    missing = [k for k in keys if k not in data]
    if missing:
        raise PmfError(f"JSON 缺少字段: {', '.join(missing)}")


def load_json(path: PathLike) -> dict[str, Any]:
    src = Path(path)
    with src.open(encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise PmfError(f"{src}: 顶层必须是 JSON object")
    return data


def detect_kind(data: Mapping[str, Any]) -> str:
    if "scale_min" in data or "scale_levels" in data or "index_space" in data:
        return "gaussian"
    if "qp_num" in data or "channels" in data:
        return "bitest"
    raise PmfError("无法判断 PMF 类型：需要 gaussian(scale_min/scale_levels) 或 bitest(qp_num/channels)")


def json_to_pmf1(data: Mapping[str, Any], *, kind: str | None = None) -> bytes:
    kind = kind or detect_kind(data)
    _require(data, "pmf_lengths", "pmf_offsets", "pmf_table")
    lengths = _as_int_list("pmf_lengths", data["pmf_lengths"])
    offsets = _as_int_list("pmf_offsets", data["pmf_offsets"])
    table = _as_int_list("pmf_table", data["pmf_table"])

    chunks = [PMF_MAGIC]
    chunks.append(_U32.pack(len(lengths)))
    chunks.append(_U32.pack(len(offsets)))
    chunks.append(_U32.pack(len(table)))
    chunks.append(b"".join(_I32.pack(v) for v in lengths))
    chunks.append(b"".join(_I32.pack(v) for v in offsets))
    chunks.append(b"".join(_I32.pack(v) for v in table))

    if kind == "gaussian":
        _require(data, "scale_min", "scale_max", "scale_levels")
        index_space = data.get("index_space", False)
        chunks.append(_U32.pack(TAG_GAUSSIAN))
        chunks.append(_F64.pack(float(data["scale_min"])))
        chunks.append(_F64.pack(float(data["scale_max"])))
        chunks.append(_U32.pack(int(data["scale_levels"])))
        chunks.append(_U32.pack(1 if index_space else 0))
    elif kind == "bitest":
        _require(data, "qp_num", "channels")
        qp_num = int(data["qp_num"])
        channels = int(data["channels"])
        if qp_num * channels != len(lengths):
            raise PmfError(
                f"bitest 尺寸不一致: qp_num*{channels}={qp_num * channels} != len(pmf_lengths)={len(lengths)}"
            )
        chunks.append(_U32.pack(TAG_BITEST))
        chunks.append(_U32.pack(qp_num))
        chunks.append(_U32.pack(channels))
    else:
        raise PmfError(f"未知 PMF 类型: {kind}")

    return b"".join(chunks)


def pmf1_to_dict(blob: bytes) -> dict[str, Any]:
    if len(blob) < 16 or blob[:4] != PMF_MAGIC:
        raise PmfError("不是 PMF1 文件")
    nL, nO, nT = struct.unpack_from("<III", blob, 4)
    off = 16
    need = off + (nL + nO + nT) * 4 + 4
    if len(blob) < need:
        raise PmfError("PMF1 文件截断")

    def _take_i32(count: int) -> list[int]:
        nonlocal off
        vals = list(struct.unpack_from("<" + "i" * count, blob, off))
        off += count * 4
        return vals

    lengths = _take_i32(nL)
    offsets = _take_i32(nO)
    table = _take_i32(nT)
    tag = struct.unpack_from("<I", blob, off)[0]
    off += 4
    base = {
        "pmf_lengths": lengths,
        "pmf_offsets": offsets,
        "pmf_table": table,
    }
    if tag == TAG_GAUSSIAN:
        if len(blob) < off + 24:
            raise PmfError("gaussian 尾部截断")
        scale_min, scale_max = struct.unpack_from("<dd", blob, off)
        scale_levels, index_space = struct.unpack_from("<II", blob, off + 16)
        base.update(
            {
                "kind": "gaussian",
                "scale_min": scale_min,
                "scale_max": scale_max,
                "scale_levels": scale_levels,
                "index_space": bool(index_space),
            }
        )
        return base
    if tag == TAG_BITEST:
        if len(blob) < off + 8:
            raise PmfError("bitest 尾部截断")
        qp_num, channels = struct.unpack_from("<II", blob, off)
        base.update({"kind": "bitest", "qp_num": qp_num, "channels": channels})
        return base
    raise PmfError(f"未知 PMF1 tag={tag}")


def convert_json_file(src: PathLike, dst: PathLike, *, kind: str | None = None) -> dict[str, Any]:
    data = load_json(src)
    resolved = kind or detect_kind(data)
    blob = json_to_pmf1(data, kind=resolved)
    out = Path(dst)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    meta = pmf1_to_dict(blob)
    meta["source"] = str(Path(src))
    meta["output"] = str(out)
    meta["bytes"] = len(blob)
    return meta
