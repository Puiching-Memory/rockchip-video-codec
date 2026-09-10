#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""RKMDL1 模型容器打包/校验（写入侧与读取侧，纯 stdlib）。

版式与 ``core/src/rkmodel.cpp`` 逐字节对齐（小端；格式版本 1；
旧文件不适配，一律用当前工具重出）：

- [0:8] magic ``b"RKMDL1\x00\x00"``；[8:12] u32 header_size = 128；
  [12:16] u32 载荷数；[16:48] id（32B，NUL 填充，有效长度 ≤31）；
  [48:64] family、[64:80] role、[80:96] target（各 16B，有效长度 ≤15）；
  [96:128] 保留零。
- 表项 @128+i*88：kind（32B，≤31）+ u32 flags + u32 保留 0 +
  u64 offset + u64 length + 32B sha256（载荷字节哈希）。
- 载荷数据按表序依次拼接。

同一文件允许出现多个 ``qppatch`` 载荷（多 rung CBR 需要）；
C++ ``unpack_model`` 同样放行（``find`` 取首个，补丁收集走全表扫描）。
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = b"RKMDL1\x00\x00"
HEADER_SIZE = 128
ENTRY_SIZE = 88
MAX_PAYLOADS = 16
MAX_FILE = 1 << 29

_OFF_ID = 16
_OFF_FAMILY = 48
_OFF_ROLE = 64
_OFF_TARGET = 80
_LEN_ID = 32
_LEN_WORD = 16
_LEN_KIND = 32


class RkmdlError(RuntimeError):
    pass


def _encode_str(value: str, cap: int, what: str) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) >= cap:
        raise RkmdlError(f"{what} 过长（{len(raw)}B，上限 {cap - 1}B）: {value!r}")
    return raw + b"\x00" * (cap - len(raw))


def _decode_str(buf: bytes, what: str) -> str:
    try:
        end = buf.index(b"\x00")
    except ValueError:
        raise RkmdlError(f"{what} 缺少 NUL 终止") from None
    return buf[:end].decode("utf-8")


def pack_model(meta: dict, payloads: list) -> bytes:
    """meta 需含 id/family/role/target；payloads 为 (kind, bytes[, flags])。"""
    for key in ("id", "family", "role", "target"):
        if not meta.get(key):
            raise RkmdlError(f"meta 缺少 {key}")
    if len(payloads) > MAX_PAYLOADS:
        raise RkmdlError(f"载荷数 {len(payloads)} 超过 {MAX_PAYLOADS}")
    total = HEADER_SIZE + ENTRY_SIZE * len(payloads)
    for item in payloads:
        total += len(item[1])
        if total > MAX_FILE:
            raise RkmdlError("模型超过 512MiB 上限")
    out = bytearray(total)
    out[0:8] = MAGIC
    struct.pack_into("<II", out, 8, HEADER_SIZE, len(payloads))
    out[_OFF_ID:_OFF_ID + _LEN_ID] = _encode_str(meta["id"], _LEN_ID, "id")
    out[_OFF_FAMILY:_OFF_FAMILY + _LEN_WORD] = _encode_str(
        meta["family"], _LEN_WORD, "family")
    out[_OFF_ROLE:_OFF_ROLE + _LEN_WORD] = _encode_str(
        meta["role"], _LEN_WORD, "role")
    out[_OFF_TARGET:_OFF_TARGET + _LEN_WORD] = _encode_str(
        meta["target"], _LEN_WORD, "target")
    off = HEADER_SIZE + ENTRY_SIZE * len(payloads)
    for i, item in enumerate(payloads):
        kind, data = item[0], item[1]
        flags = item[2] if len(item) > 2 else 0
        e = HEADER_SIZE + i * ENTRY_SIZE
        out[e:e + _LEN_KIND] = _encode_str(kind, _LEN_KIND, "kind")
        struct.pack_into("<IIQQ", out, e + 32, flags, 0, off, len(data))
        out[e + 56:e + 88] = hashlib.sha256(data).digest()
        out[off:off + len(data)] = data
        off += len(data)
    return bytes(out)


def unpack_model(data: bytes):
    """返回 (meta, [(kind, flags, bytes), ...])；版式/哈希不对抛错。"""
    if len(data) < HEADER_SIZE:
        raise RkmdlError("文件小于 128B 头")
    if data[0:8] != MAGIC:
        raise RkmdlError(f"bad magic {data[0:8]!r}")
    header_size, count = struct.unpack_from("<II", data, 8)
    if header_size != HEADER_SIZE:
        raise RkmdlError(f"bad header size {header_size}")
    if count > MAX_PAYLOADS:
        raise RkmdlError(f"载荷数 {count} 越界")
    if any(data[96:128]):
        raise RkmdlError("保留字段非零")
    if len(data) < HEADER_SIZE + ENTRY_SIZE * count:
        raise RkmdlError("表项区截断")
    if len(data) > MAX_FILE:
        raise RkmdlError("文件超过 512MiB 上限")
    meta = {
        "id": _decode_str(data[16:48], "id"),
        "family": _decode_str(data[48:64], "family"),
        "role": _decode_str(data[64:80], "role"),
        "target": _decode_str(data[80:96], "target"),
    }
    payloads = []
    prev_end = HEADER_SIZE + ENTRY_SIZE * count
    for i in range(count):
        e = HEADER_SIZE + i * ENTRY_SIZE
        kind = _decode_str(data[e:e + 32], "kind")
        if not kind:
            raise RkmdlError("空 kind")
        flags, reserved, off, length = struct.unpack_from("<IIQQ", data, e + 32)
        if reserved:
            raise RkmdlError("表项保留字段非零")
        if length > MAX_FILE or off > len(data) or length > len(data) - off:
            raise RkmdlError(f"载荷 {kind} 越界")
        if off < prev_end:
            raise RkmdlError(f"载荷 {kind} 重叠")
        prev_end = off + length
        blob = data[off:off + length]
        if hashlib.sha256(blob).digest() != data[e + 56:e + 88]:
            raise RkmdlError(f"载荷 {kind} 哈希 mismatch")
        payloads.append((kind, flags, blob))
    return meta, payloads


def verify_file(path) -> dict:
    data = Path(path).read_bytes()
    meta, payloads = unpack_model(data)
    return {"path": str(path), "bytes": len(data), **meta,
            "payloads": [(k, len(b)) for k, _, b in payloads]}


def _cmd_pack(ns: argparse.Namespace) -> int:
    payloads = []
    for spec in ns.payload:
        kind, _, path = spec.partition("=")
        if not kind or not path:
            raise RkmdlError(f"--payload 格式应为 kind=path: {spec!r}")
        payloads.append((kind, Path(path).read_bytes()))
    blob = pack_model({"id": ns.id, "family": ns.family, "role": ns.role,
                       "target": ns.target}, payloads)
    ns.out.parent.mkdir(parents=True, exist_ok=True)
    ns.out.write_bytes(blob)
    print(f"{ns.out}  {len(blob)} B  {len(payloads)} payloads")
    return 0


def _cmd_info(ns: argparse.Namespace) -> int:
    info = verify_file(ns.file)
    print(f"{info['path']}  {info['bytes']} B")
    print(f"  id={info['id']} family={info['family']} role={info['role']} "
          f"target={info['target']}")
    for kind, size in info["payloads"]:
        print(f"  {kind:16s} {size:10d} B")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="RKMDL1 容器打包/校验")
    sub = p.add_subparsers(dest="cmd", required=True)
    pk = sub.add_parser("pack", help="按 kind=path 打包")
    pk.add_argument("--id", required=True)
    pk.add_argument("--family", required=True)
    pk.add_argument("--role", required=True)
    pk.add_argument("--target", required=True)
    pk.add_argument("--payload", action="append", default=[],
                    help="kind=path，可重复（qppatch 可出现多次）")
    pk.add_argument("--out", type=Path, required=True)
    pk.set_defaults(func=_cmd_pack)
    for name in ("verify", "info"):
        c = sub.add_parser(name, help="校验并打印模型信息")
        c.add_argument("file", type=Path)
        c.set_defaults(func=_cmd_info)
    return p


def main(argv=None) -> int:
    ns = build_parser().parse_args(argv)
    try:
        return ns.func(ns)
    except RkmdlError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
