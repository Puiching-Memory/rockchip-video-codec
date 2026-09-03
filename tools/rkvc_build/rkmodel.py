"""`.rkmodel` v1 容器打包/检查工具（写入侧）。

格式与 ``lib/rkmodel_layout.h`` 一致：固定头 64B、TLV 区、每项 24B
的载荷表，以及载荷数据。
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0x464D4B52  # "RKMF" LE
VERSION = 1
FIXED_SIZE = 64
MAX_HEADER_SIZE = 1 << 20
MAX_PAYLOADS = 16
MAX_PAYLOAD_SIZE = 1 << 30
PAYLOAD_ENTRY_SIZE = 24

TAG_FAMILY = 1
TAG_ROLE = 2
TAG_ID = 3
TAG_VERSION = 4
TAG_RKNN_TARGET = 5
TAG_MIN_ABI = 8

PAYLOAD_KINDS = {"rknn": 1, "pmf": 2, "qppatch": 3,
                 "pmf-gaussian": 4, "pmf-bitest": 5}
KIND_NAMES = {value: name for name, value in PAYLOAD_KINDS.items()}


def _parse(data: bytes):
    if len(data) < FIXED_SIZE:
        raise ValueError("文件小于固定头")
    magic, version, header_len, payload_count, entry_size = struct.unpack_from(
        "<IIIII", data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    if version != VERSION:
        raise ValueError(f"不支持的格式版本 {version}")
    if entry_size != PAYLOAD_ENTRY_SIZE:
        raise ValueError(f"不支持的载荷表项大小: {entry_size}")
    if any(data[20:FIXED_SIZE]):
        raise ValueError("固定头保留字段非零")
    if header_len > MAX_HEADER_SIZE:
        raise ValueError(f"TLV 区超过上限: {header_len}")
    if not 1 <= payload_count <= MAX_PAYLOADS:
        raise ValueError(f"载荷数越界: {payload_count}")

    table_offset = FIXED_SIZE + header_len
    data_offset = table_offset + payload_count * PAYLOAD_ENTRY_SIZE
    if data_offset > len(data):
        raise ValueError("TLV 或载荷表超出文件")
    tlv = data[FIXED_SIZE:table_offset]
    entries = []
    kinds: set[int] = set()
    for index in range(payload_count):
        kind, flags, offset, length = struct.unpack_from(
            "<IIQQ", data, table_offset + index * PAYLOAD_ENTRY_SIZE)
        if not 1 <= kind <= 31 or kind in kinds:
            raise ValueError(f"载荷类型无效或重复: {kind}")
        if flags:
            raise ValueError(f"不支持的载荷 flags: 0x{flags:08x}")
        if offset < data_offset or offset > len(data) or \
                length > len(data) - offset or length > MAX_PAYLOAD_SIZE:
            raise ValueError(f"载荷 {kind} 超出文件边界")
        end = offset + length
        if any(offset < old_end and old_offset < end
               for _, _, old_offset, old_length in entries
               for old_end in (old_offset + old_length,)):
            raise ValueError("载荷范围重叠")
        entries.append((kind, flags, offset, length))
        kinds.add(kind)
    payloads = [data[offset:offset + length]
                for _, _, offset, length in entries]
    return version, tlv, entries, payloads


def _tlv(tag: int, value: bytes) -> bytes:
    return struct.pack("<HI", tag, len(value)) + value


def pack(args: argparse.Namespace) -> int:
    if not args.payload:
        print("error: 至少需要一个 --payload kind=path", file=sys.stderr)
        return 2
    if len(args.payload) > MAX_PAYLOADS:
        print(f"error: 载荷数超过上界 {MAX_PAYLOADS}", file=sys.stderr)
        return 2

    tlv = bytearray()
    tlv += _tlv(TAG_FAMILY, args.family.encode())
    tlv += _tlv(TAG_ROLE, args.role.encode())
    tlv += _tlv(TAG_ID, args.id.encode())
    tlv += _tlv(TAG_VERSION, args.version.encode())
    if args.rknn_target:
        tlv += _tlv(TAG_RKNN_TARGET, args.rknn_target.encode())
    if args.min_abi:
        tlv += _tlv(TAG_MIN_ABI, struct.pack("<I", args.min_abi))

    payloads: list[tuple[int, bytes]] = []
    seen_kinds: set[int] = set()
    for spec in args.payload:
        kind_name, separator, path_text = spec.partition("=")
        kind = PAYLOAD_KINDS.get(kind_name)
        if kind is None or not separator or not path_text:
            print(f"error: 无效载荷说明 {spec!r}（kind 之一："
                  f"{sorted(PAYLOAD_KINDS)}）", file=sys.stderr)
            return 2
        if kind in seen_kinds:
            print(f"error: 重复载荷类型 {kind_name}", file=sys.stderr)
            return 2
        data = Path(path_text).read_bytes()
        if len(data) > MAX_PAYLOAD_SIZE:
            print(f"error: 载荷超过 1 GiB: {path_text}", file=sys.stderr)
            return 2
        payloads.append((kind, data))
        seen_kinds.add(kind)

    data_offset = FIXED_SIZE + len(tlv) + len(payloads) * PAYLOAD_ENTRY_SIZE
    entries = bytearray()
    body = bytearray()
    for kind, data in payloads:
        entries += struct.pack("<IIQQ", kind, 0,
                               data_offset + len(body), len(data))
        body += data

    fixed = struct.pack("<IIIII", MAGIC, VERSION, len(tlv), len(payloads),
                        PAYLOAD_ENTRY_SIZE)
    fixed += b"\0" * (FIXED_SIZE - len(fixed))
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(fixed + tlv + entries + body)
    print(f"packed: {output} (payloads={len(payloads)}, tlv={len(tlv)}B, "
          f"total={FIXED_SIZE + len(tlv) + len(entries) + len(body)}B)")
    return 0


def _read_tlv(buf: bytes, fields: dict) -> None:
    pos = 0
    names = {TAG_FAMILY: "family", TAG_ROLE: "role", TAG_ID: "id",
             TAG_VERSION: "version", TAG_RKNN_TARGET: "rknn_target",
             TAG_MIN_ABI: "min_abi"}
    while pos + 6 <= len(buf):
        tag, length = struct.unpack_from("<HI", buf, pos)
        pos += 6
        if pos + length > len(buf):
            raise ValueError(f"TLV tag {tag} overruns header")
        value = buf[pos:pos + length]
        if tag == TAG_MIN_ABI:
            if length != 4:
                raise ValueError("min_abi TLV 长度必须为 4")
            fields[names[tag]] = struct.unpack("<I", value)[0]
        elif tag in names:
            fields[names[tag]] = value.decode(errors="replace")
        pos += length
    if pos != len(buf):
        raise ValueError(f"TLV 区尾部残留 {len(buf) - pos} 字节")


def inspect(args: argparse.Namespace) -> int:
    try:
        version, tlv, entries, _ = _parse(Path(args.file).read_bytes())
        fields: dict = {}
        _read_tlv(tlv, fields)
        missing = {"id", "family", "role", "version"} - fields.keys()
        if missing:
            raise ValueError(f"缺少必填 TLV: {', '.join(sorted(missing))}")
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"format: v{version}  payloads={len(entries)}")
    for key in ("id", "family", "role", "version", "rknn_target", "min_abi"):
        if key in fields:
            print(f"{key}: {fields[key]}")
    for index, (kind, _flags, offset, length) in enumerate(entries):
        print(f"payload[{index}]: {KIND_NAMES.get(kind, kind)} "
              f"off={offset} len={length}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="rkmodel", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    command = sub.add_parser("pack", help="打包 .rkmodel 容器")
    command.add_argument("output")
    command.add_argument("--id", required=True)
    command.add_argument("--family", required=True)
    command.add_argument("--role", required=True)
    command.add_argument("--version", required=True)
    command.add_argument("--rknn-target")
    command.add_argument("--min-abi", type=int)
    command.add_argument("--payload", action="append", metavar="KIND=PATH")
    command.set_defaults(func=pack)

    command = sub.add_parser("inspect", help="检查 .rkmodel 容器")
    command.add_argument("file")
    command.set_defaults(func=inspect)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
