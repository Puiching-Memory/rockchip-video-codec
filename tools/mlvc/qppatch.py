# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""QPP1：基座 RKNN + 每 QP 差异区间。应用后与目标文件逐字节相同。"""

from __future__ import annotations

import json
import re
import struct
import zlib
from pathlib import Path
from typing import Sequence, Union

PathLike = Union[str, Path]

MAGIC = b"QPP1"
VERSION = 1
HEADER_SIZE = 48
HEADER_FMT = "<4sIQIIIIII8s"  # 48 B
RANGE_FMT = "<II"
DEFAULT_COALESCE_GAP = 64
QP_DIR_RE = re.compile(r"^qp(\d+)$")


class QppatchError(ValueError):
    pass


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def diff_ranges(base: bytes, target: bytes, *, gap: int = DEFAULT_COALESCE_GAP) -> list[tuple[int, int]]:
    if len(base) != len(target):
        raise QppatchError(f"基座与目标大小不同: {len(base)} vs {len(target)}")
    if len(base) > 0xFFFFFFFF:
        raise QppatchError("文件超过 4GiB，QPP1 使用 uint32 偏移")
    raw: list[list[int]] = []
    i = 0
    n = len(base)
    while i < n:
        if base[i] != target[i]:
            j = i + 1
            while j < n and base[j] != target[j]:
                j += 1
            raw.append([i, j - i])
            i = j
        else:
            i += 1
    if not raw:
        return []
    merged = [raw[0]]
    for off, ln in raw[1:]:
        prev_off, prev_ln = merged[-1]
        prev_end = prev_off + prev_ln
        if off - prev_end <= gap:
            merged[-1] = [prev_off, off + ln - prev_off]
        else:
            merged.append([off, ln])
    return [(int(o), int(l)) for o, l in merged]


def encode(base: bytes, target: bytes, qp: int, *, gap: int = DEFAULT_COALESCE_GAP) -> bytes:
    if qp < 0:
        raise QppatchError(f"qp 不能为负: {qp}")
    ranges = diff_ranges(base, target, gap=gap)
    payload = b"".join(target[off : off + ln] for off, ln in ranges)
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        VERSION,
        len(base),
        int(qp),
        len(ranges),
        0,
        crc32(base),
        crc32(payload),
        int(gap),
        b"\x00" * 8,
    )
    body = b"".join(struct.pack(RANGE_FMT, off, ln) for off, ln in ranges)
    blob = header + body + payload
    if len(header) != HEADER_SIZE:
        raise QppatchError(f"内部错误: header={len(header)}")
    return blob


def decode_header(blob: bytes) -> dict:
    if len(blob) < HEADER_SIZE:
        raise QppatchError("补丁截断")
    magic, ver, base_size, qp, nrange, flags, base_crc, payload_crc, gap, _res = struct.unpack(
        HEADER_FMT, blob[:HEADER_SIZE]
    )
    if magic != MAGIC:
        raise QppatchError("不是 QPP1 文件")
    if ver != VERSION:
        raise QppatchError(f"不支持的 QPP1 版本 {ver}")
    return {
        "base_size": int(base_size),
        "qp": int(qp),
        "num_ranges": int(nrange),
        "flags": int(flags),
        "base_crc32": int(base_crc),
        "payload_crc32": int(payload_crc),
        "coalesce_gap": int(gap),
    }


def apply(base: bytes, blob: bytes, *, expected_qp: int | None = None) -> bytes:
    meta = decode_header(blob)
    if meta["flags"] != 0:
        raise QppatchError(f"未知 flags={meta['flags']}")
    if expected_qp is not None and meta["qp"] != expected_qp:
        raise QppatchError(f"补丁 qp={meta['qp']}，期望 {expected_qp}")
    if meta["base_size"] != len(base):
        raise QppatchError("基座大小与补丁声明不一致")
    if crc32(base) != meta["base_crc32"]:
        raise QppatchError("基座 CRC32 不匹配（拿错基座或 toolkit 版本不同）")
    nrange = meta["num_ranges"]
    ranges_off = HEADER_SIZE
    payload_off = HEADER_SIZE + nrange * 8
    if len(blob) < payload_off:
        raise QppatchError("区间表截断")
    out = bytearray(base)
    cursor = payload_off
    for i in range(nrange):
        off, ln = struct.unpack_from(RANGE_FMT, blob, ranges_off + i * 8)
        if off + ln > len(out):
            raise QppatchError(f"区间越界 offset={off} length={ln}")
        chunk = blob[cursor : cursor + ln]
        if len(chunk) != ln:
            raise QppatchError("payload 截断")
        out[off : off + ln] = chunk
        cursor += ln
    if cursor != len(blob):
        raise QppatchError("payload 长度与区间合计不符")
    if crc32(blob[payload_off:]) != meta["payload_crc32"]:
        raise QppatchError("payload CRC32 不匹配")
    return bytes(out)


def write_patch(path: PathLike, base: bytes, target: bytes, qp: int, *, gap: int = DEFAULT_COALESCE_GAP) -> dict:
    blob = encode(base, target, qp, gap=gap)
    restored = apply(base, blob, expected_qp=qp)
    if restored != target:
        raise QppatchError(f"自检失败: qp={qp} 应用后与目标不一致")
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    meta = decode_header(blob)
    meta["bytes"] = len(blob)
    meta["output"] = str(out)
    return meta


def _first_rknn(directory: Path, kind: str) -> Path | None:
    key = "Encoder" if kind == "enc" else "Decoder"
    hits = sorted(p for p in directory.glob("*.rknn") if key.lower() in p.name.lower())
    return hits[0] if hits else None


def scan_qp_models(models_dir: PathLike) -> dict[str, dict[int, Path]]:
    root = Path(models_dir)
    if not root.is_dir():
        raise QppatchError(f"模型目录不存在: {root}")
    found: dict[str, dict[int, Path]] = {"enc": {}, "dec": {}}
    for child in sorted(root.iterdir()):
        if not child.is_dir():
            continue
        m = QP_DIR_RE.match(child.name)
        if not m:
            continue
        qp = int(m.group(1))
        enc = _first_rknn(child, "enc")
        dec = _first_rknn(child, "dec")
        if enc:
            found["enc"][qp] = enc
        if dec:
            found["dec"][qp] = dec
    if not found["enc"] and not found["dec"]:
        raise QppatchError(f"{root} 下没有 qpXX/*.rknn")
    return found


def generate_from_qp_dir(
    models_dir: PathLike,
    out_dir: PathLike,
    *,
    base_qp: int = 21,
    gap: int = DEFAULT_COALESCE_GAP,
    parts: Sequence[str] = ("enc", "dec"),
) -> dict:
    found = scan_qp_models(models_dir)
    dest = Path(out_dir)
    dest.mkdir(parents=True, exist_ok=True)
    summary: dict = {"base_qp": base_qp, "coalesce_gap": gap, "parts": {}}
    for part in parts:
        qps = found.get(part) or {}
        if base_qp not in qps:
            raise QppatchError(f"{part} 缺少基座 qp{base_qp}")
        base_path = qps[base_qp]
        base = base_path.read_bytes()
        part_meta = {"base": str(base_path), "base_bytes": len(base), "patches": {}}
        for qp in sorted(qps):
            target_path = qps[qp]
            target = target_path.read_bytes()
            if len(target) != len(base):
                raise QppatchError(
                    f"{part} qp{qp} 大小 {len(target)} ≠ 基座 {len(base)}，无法打补丁"
                )
            name = f"{part}_qp{qp}.qppatch"
            meta = write_patch(dest / name, base, target, qp, gap=gap)
            part_meta["patches"][str(qp)] = meta
            print(
                f"  {name:24s}  {meta['bytes']:8d} B  "
                f"ranges={meta['num_ranges']}  qp={qp}"
            )
        summary["parts"][part] = part_meta
    (dest / "qppatch_manifest.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return summary
