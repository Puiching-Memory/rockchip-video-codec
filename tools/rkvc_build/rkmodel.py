"""`.rkmodel` v1 容器打包/检查工具（写入侧）。

与 lib/rkmodel_layout.h 保持同一格式来源：固定头 64B + TLV 区 + 载荷表
（count x 56B，含 SHA-256）+ 可选签名尾（84B）+ 载荷数据。

用法：
    python3 -m rkvc_build.rkmodel pack OUT.rkmodel --id ID --family sr \
        --role upscale --version 1.2.3 [--rknn-target rk3588] \
        [--min-abi 1024] --payload rknn=model.rknn [--payload pmf=table.bin]
    python3 -m rkvc_build.rkmodel inspect FILE.rkmodel [--verify-payloads]

签名由生产侧密钥槽完成；本工具产出未签名容器（trust=unsigned）。
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import struct
import sys
from pathlib import Path

MAGIC = 0x464D4B52  # "RKMF" LE
VERSION = 1
FIXED_SIZE = 64
PAYLOAD_ENTRY_SIZE = 56
SIG_TRAILER_SIZE = 84

TAG_FAMILY = 1
TAG_ROLE = 2
TAG_ID = 3
TAG_VERSION = 4
TAG_RKNN_TARGET = 5
TAG_MIN_ABI = 8

PAYLOAD_KINDS = {"rknn": 1, "pmf": 2, "qppatch": 3}
KIND_NAMES = {v: k for k, v in PAYLOAD_KINDS.items()}

SIG_ALG_ED25519 = 1
FLAG_SIGNED = 1


def _load_sodium() -> ctypes.CDLL:
    for soname in ("libsodium.so.23", "libsodium.so"):
        try:
            lib = ctypes.CDLL(soname)
            lib.sodium_init()
            return lib
        except OSError:
            continue
    raise RuntimeError("libsodium 运行库不可用（需 libsodium23+）")


def keygen(args: argparse.Namespace) -> int:
    """生成 Ed25519 信任根密钥对。.sec 永不入库；.pub 用于编译期固定。"""
    lib = _load_sodium()
    seed = (bytes.fromhex(args.seed) if args.seed
            else os.urandom(32))
    if len(seed) != 32:
        print("error: seed 必须是 32 字节 hex（64 字符）", file=sys.stderr)
        return 2
    pk = ctypes.create_string_buffer(32)
    sk = ctypes.create_string_buffer(64)
    lib.crypto_sign_ed25519_seed_keypair(pk, sk, seed)
    key_id = hashlib.sha256(pk.raw).digest()[:16].hex()
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    sec_path = out.with_suffix(".sec")
    pub_path = out.with_suffix(".pub")
    sec_path.write_text(sk.raw.hex() + "\n")
    os.chmod(sec_path, 0o600)
    pub_path.write_text(f"# key_id: {key_id}\n{pk.raw.hex()}\n")
    print(f"keygen: {pub_path} (key_id={key_id})  sec={sec_path} [mode 600, 勿入库]")
    return 0


def _parse(data: bytes):
    if len(data) < FIXED_SIZE:
        raise ValueError("文件小于固定头")
    magic, version, hlen, pcount, flags = struct.unpack_from("<IIIII", data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    tlv = data[FIXED_SIZE:FIXED_SIZE + hlen]
    table_off = FIXED_SIZE + hlen
    signed = bool(flags & FLAG_SIGNED)
    sig_off = table_off + pcount * PAYLOAD_ENTRY_SIZE
    entries = []
    for i in range(pcount):
        kind, eflags, off, length = struct.unpack_from(
            "<IIQQ", data, table_off + i * PAYLOAD_ENTRY_SIZE)
        digest = data[table_off + i * PAYLOAD_ENTRY_SIZE + 24:
                      table_off + i * PAYLOAD_ENTRY_SIZE + 56]
        entries.append((kind, eflags, off, length, digest))
    base = sig_off + (SIG_TRAILER_SIZE if signed else 0)
    payloads = [data[off:off + length] for _, _, off, length, _ in entries]
    trailer = data[sig_off:base] if signed else b""
    return version, flags, tlv, entries, trailer, payloads


def sign(args: argparse.Namespace) -> int:
    """对 .rkmodel 附加/替换 Ed25519 签名尾（重排载荷偏移）。"""
    lib = _load_sodium()
    sk = bytes.fromhex(Path(args.key).read_text().strip())
    if len(sk) != 64:
        print("error: 私钥文件必须是 64 字节 hex", file=sys.stderr)
        return 2
    pk = sk[32:]
    path = Path(args.file)
    data = path.read_bytes()
    try:
        version, flags, tlv, entries, _tr, payloads = _parse(data)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    table_off = FIXED_SIZE + len(tlv)
    data_off = table_off + len(entries) * PAYLOAD_ENTRY_SIZE + SIG_TRAILER_SIZE
    table = bytearray()
    cursor = data_off
    for (kind, eflags, _off, _len, digest), pdata in zip(entries, payloads):
        table += struct.pack("<IIQQ", kind, eflags, cursor, len(pdata))
        table += digest
        cursor += len(pdata)

    fixed = struct.pack("<IIIII", MAGIC, version, len(tlv), len(entries),
                        flags | FLAG_SIGNED)
    fixed += b"\0" * (FIXED_SIZE - len(fixed))
    sig_input = fixed + bytes(tlv) + bytes(table)
    sig = ctypes.create_string_buffer(64)
    siglen = ctypes.c_ulonglong(0)
    lib.crypto_sign_ed25519_detached(sig, ctypes.byref(siglen),
                                     sig_input, len(sig_input), sk)
    trailer = struct.pack("<I", SIG_ALG_ED25519) + \
        hashlib.sha256(pk).digest()[:16] + sig.raw
    path.write_bytes(sig_input + trailer + b"".join(payloads))
    print(f"signed: {path} (key_id={hashlib.sha256(pk).digest()[:16].hex()})")
    return 0


def verify(args: argparse.Namespace) -> int:
    """用 .pub 验证 .rkmodel 签名。"""
    lib = _load_sodium()
    lines = [ln for ln in Path(args.pubkey).read_text().splitlines()
             if ln and not ln.startswith("#")]
    pk = bytes.fromhex(lines[0].strip())
    if len(pk) != 32:
        print("error: 公钥必须是 32 字节 hex", file=sys.stderr)
        return 2
    data = Path(args.file).read_bytes()
    try:
        version, flags, tlv, entries, trailer, _ = _parse(data)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if not flags & FLAG_SIGNED:
        print("error: 容器未签名", file=sys.stderr)
        return 1
    alg, key_id = struct.unpack_from("<I", trailer, 0)[0], trailer[4:20]
    if alg != SIG_ALG_ED25519:
        print(f"error: 不支持的签名算法 {alg}", file=sys.stderr)
        return 1
    if key_id != hashlib.sha256(pk).digest()[:16]:
        print("error: key_id 与公钥不匹配", file=sys.stderr)
        return 1
    table = bytearray()
    for kind, eflags, off, length, digest in entries:
        table += struct.pack("<IIQQ", kind, eflags, off, length)
        table += digest
    fixed = struct.pack("<IIIII", MAGIC, version, len(tlv), len(entries), flags)
    fixed += b"\0" * (FIXED_SIZE - len(fixed))
    sig_input = fixed + bytes(tlv) + bytes(table)
    rc = lib.crypto_sign_ed25519_verify_detached(
        trailer[20:84], sig_input, len(sig_input), pk)
    if rc != 0:
        print("error: 签名验证失败", file=sys.stderr)
        return 1
    print(f"verify OK: key_id={key_id.hex()}")
    return 0


def pack(args: argparse.Namespace) -> int:
    if args.payload is None or len(args.payload) == 0:
        print("error: 至少需要一个 --payload kind=path", file=sys.stderr)
        return 2
    if len(args.payload) > 16:
        print("error: 载荷数超过上界 16", file=sys.stderr)
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

    payloads = []
    for spec in args.payload:
        kind_s, _, path_s = spec.partition("=")
        kind = PAYLOAD_KINDS.get(kind_s)
        if kind is None or not path_s:
            print(f"error: 无效载荷说明 {spec!r}（kind 之一：{sorted(PAYLOAD_KINDS)}）",
                  file=sys.stderr)
            return 2
        data = Path(path_s).read_bytes()
        payloads.append((kind, data, hashlib.sha256(data).digest()))

    table_off = FIXED_SIZE + len(tlv)
    data_off = table_off + len(payloads) * PAYLOAD_ENTRY_SIZE
    entries = bytearray()
    blob = bytearray()
    for kind, data, digest in payloads:
        entries += struct.pack("<IIQQ", kind, 0, data_off + len(blob),
                               len(data))
        entries += digest
        blob += data

    fixed = struct.pack("<IIIII", MAGIC, VERSION, len(tlv), len(payloads), 0)
    fixed += b"\0" * (FIXED_SIZE - len(fixed))

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(fixed + bytes(tlv) + bytes(entries) + bytes(blob))
    print(f"packed: {out} (payloads={len(payloads)}, tlv={len(tlv)}B, "
          f"total={FIXED_SIZE + len(tlv) + len(entries) + len(blob)}B)")
    return 0


def _tlv(tag: int, value: bytes) -> bytes:
    return struct.pack("<HI", tag, len(value)) + value


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
        if tag in names:
            fields[names[tag]] = (struct.unpack("<I", value)[0]
                                  if tag == TAG_MIN_ABI
                                  else value.decode(errors="replace"))
        pos += length
    if pos != len(buf):
        raise ValueError(f"trailing {len(buf) - pos} bytes in TLV region")


def inspect(args: argparse.Namespace) -> int:
    data = Path(args.file).read_bytes()
    if len(data) < FIXED_SIZE:
        print("error: 文件小于固定头", file=sys.stderr)
        return 1
    magic, version, hlen, pcount, flags = struct.unpack_from("<IIIII", data, 0)
    if magic != MAGIC:
        print(f"error: bad magic 0x{magic:08x}", file=sys.stderr)
        return 1
    fields: dict = {}
    try:
        _read_tlv(data[FIXED_SIZE:FIXED_SIZE + hlen], fields)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"format: v{version}  payloads={pcount}  signed={'yes' if flags & 1 else 'no'}")
    for key in ("id", "family", "role", "version", "rknn_target", "min_abi"):
        if key in fields:
            print(f"{key}: {fields[key]}")
    table_off = FIXED_SIZE + hlen
    for i in range(pcount):
        kind, _f, off, length = struct.unpack_from(
            "<IIQQ", data, table_off + i * PAYLOAD_ENTRY_SIZE)
        digest = data[table_off + i * PAYLOAD_ENTRY_SIZE + 24:
                      table_off + i * PAYLOAD_ENTRY_SIZE + 56]
        line = (f"payload[{i}]: {KIND_NAMES.get(kind, kind)} off={off} "
                f"len={length} sha256={digest.hex()[:16]}...")
        if args.verify_payloads:
            actual = hashlib.sha256(data[off:off + length]).digest()
            line += "  [OK]" if actual == digest else "  [DIGEST MISMATCH]"
        print(line)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="rkmodel", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("pack", help="打包 .rkmodel 容器")
    p.add_argument("output")
    p.add_argument("--id", required=True)
    p.add_argument("--family", required=True)
    p.add_argument("--role", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--rknn-target")
    p.add_argument("--min-abi", type=int)
    p.add_argument("--payload", action="append", metavar="KIND=PATH")
    p.set_defaults(func=pack)

    p = sub.add_parser("inspect", help="检查 .rkmodel 容器")
    p.add_argument("file")
    p.add_argument("--verify-payloads", action="store_true")
    p.set_defaults(func=inspect)

    p = sub.add_parser("keygen", help="生成 Ed25519 信任根密钥对")
    p.add_argument("output", help="输出基名（生成 .pub/.sec）")
    p.add_argument("--seed", help="32 字节 hex seed（缺省随机）")
    p.set_defaults(func=keygen)

    p = sub.add_parser("sign", help="对 .rkmodel 附加/替换签名")
    p.add_argument("file")
    p.add_argument("--key", required=True, help=".sec 私钥文件")
    p.set_defaults(func=sign)

    p = sub.add_parser("verify", help="验证 .rkmodel 签名")
    p.add_argument("file")
    p.add_argument("--pubkey", required=True, help=".pub 公钥文件")
    p.set_defaults(func=verify)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
