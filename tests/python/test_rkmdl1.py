#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""RKMDL1 打包器：版式向量（对齐 core/src/rkmodel.cpp）与 roundtrip。"""

from __future__ import annotations

import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

# 测试临时目录统一落项目内 .temp/，不写系统临时目录。
_TMP_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    ".temp")
os.makedirs(_TMP_ROOT, exist_ok=True)
tempfile.tempdir = _TMP_ROOT

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "mlvc"
sys.path.insert(0, str(TOOLS))

import rkmdl1  # noqa: E402


def _meta(**kw):
    m = {"id": "t", "family": "mlvc", "role": "encoder", "target": "rk3576"}
    m.update(kw)
    return m


class PackLayoutTest(unittest.TestCase):
    def test_minimal_byte_layout(self) -> None:
        blob = rkmdl1.pack_model(_meta(id="m1"), [("rknn", b"\x01\x02\x03")])
        self.assertEqual(blob[0:8], b"RKMDL1\x00\x00")
        header_size, count = struct.unpack_from("<II", blob, 8)
        self.assertEqual((header_size, count), (128, 1))
        self.assertEqual(blob[16:18], b"m1")
        self.assertEqual(blob[18:48], b"\x00" * 30)
        self.assertEqual(blob[48:52], b"mlvc")
        self.assertEqual(blob[96:128], b"\x00" * 32)
        e = 128
        self.assertEqual(blob[e:e + 4], b"rknn")
        self.assertEqual(blob[e + 4:e + 32], b"\x00" * 28)
        flags, reserved, off, length = struct.unpack_from("<IIQQ", blob, e + 32)
        self.assertEqual((flags, reserved, off, length), (0, 0, 128 + 88, 3))
        self.assertEqual(blob[e + 56:e + 88],
                         hashlib.sha256(b"\x01\x02\x03").digest())
        self.assertEqual(blob[off:off + length], b"\x01\x02\x03")
        self.assertEqual(len(blob), off + length)

    def test_roundtrip_multi_qppatch(self) -> None:
        payloads = [("rknn", bytes(range(64))), ("pmf-gaussian", b"g" * 17),
                    ("pmf-bitest", b"b" * 5), ("qppatch", b"QPP1" + b"\x00" * 60),
                    ("qppatch", b"QPP1" + b"\x01" * 60, 7)]
        blob = rkmdl1.pack_model(_meta(id="mlvc_rk3576_qp21_encoder"), payloads)
        meta, out = rkmdl1.unpack_model(blob)
        self.assertEqual(meta["id"], "mlvc_rk3576_qp21_encoder")
        self.assertEqual(meta["role"], "encoder")
        self.assertEqual(meta["target"], "rk3576")
        self.assertEqual([(k, f, b) for k, f, b in out],
                         [("rknn", 0, bytes(range(64))), ("pmf-gaussian", 0, b"g" * 17),
                          ("pmf-bitest", 0, b"b" * 5),
                          ("qppatch", 0, b"QPP1" + b"\x00" * 60),
                          ("qppatch", 7, b"QPP1" + b"\x01" * 60)])

    def test_rejects(self) -> None:
        good = rkmdl1.pack_model(_meta(), [("rknn", b"data")])
        bad_magic = bytearray(good)
        bad_magic[0:4] = b"RKMF"
        for blob in (bytes(bad_magic), good[:100], good[:-1]):
            with self.assertRaises(rkmdl1.RkmdlError):
                rkmdl1.unpack_model(blob)
        flipped = bytearray(good)
        flipped[-1] ^= 0xFF
        with self.assertRaises(rkmdl1.RkmdlError):
            rkmdl1.unpack_model(flipped)
        reserved = bytearray(good)
        reserved[100] = 1
        with self.assertRaises(rkmdl1.RkmdlError):
            rkmdl1.unpack_model(reserved)
        with self.assertRaises(rkmdl1.RkmdlError):
            rkmdl1.pack_model(_meta(), [("k", b"x")] * 17)
        with self.assertRaises(rkmdl1.RkmdlError):
            rkmdl1.pack_model(_meta(id="x" * 32), [("rknn", b"x")])
        with self.assertRaises(rkmdl1.RkmdlError):
            rkmdl1.pack_model({"id": "x"}, [("rknn", b"x")])

    def test_pack_cli_and_verify(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.rknn").write_bytes(b"\x10" * 32)
            (root / "g.bin").write_bytes(b"\x20" * 16)
            out = root / "m.rkmodel"
            rc = rkmdl1.main(["pack", "--id", "m", "--family", "mlvc",
                              "--role", "encoder", "--target", "rk3576",
                              "--payload", f"rknn={root / 'a.rknn'}",
                              "--payload", f"pmf-gaussian={root / 'g.bin'}",
                              "--out", str(out)])
            self.assertEqual(rc, 0)
            info = rkmdl1.verify_file(out)
            self.assertEqual(info["id"], "m")
            self.assertEqual(info["payloads"], [("rknn", 32), ("pmf-gaussian", 16)])
            self.assertEqual(rkmdl1.main(["verify", str(out)]), 0)


try:
    import export_rknn  # noqa: E402
    HAS_EXPORT = True
except Exception:  # 缺 onnx 等重依赖时跳过接线测试
    HAS_EXPORT = False


@unittest.skipUnless(HAS_EXPORT, "export_rknn 不可导入")
class PackBundleTest(unittest.TestCase):
    def test_pack_bundle_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "MLVCEncoder_rk3576.rknn").write_bytes(b"E" * 64)
            (root / "MLVCDecoder_rk3576.rknn").write_bytes(b"D" * 64)
            (root / "gaussian.bin").write_bytes(b"G" * 16)
            (root / "bitest.bin").write_bytes(b"B" * 8)
            patches = root / "qp_patches"
            patches.mkdir()
            (patches / "enc_qp21.qppatch").write_bytes(b"QPP1" + b"\x00" * 60)
            (patches / "enc_qp30.qppatch").write_bytes(b"QPP1" + b"\x01" * 60)
            (patches / "dec_qp21.qppatch").write_bytes(b"QPP1" + b"\x02" * 60)
            meta = export_rknn.pack_models_bundle(
                out_dir=root, platform="rk3576", variant="mlvc",
                qp_dynamic=False, base_qp=21, models_meta={}, patch_dir=patches)
            self.assertIn("encoder", meta)
            self.assertIn("decoder", meta)
            enc = root / "mlvc_rk3576_qp21_encoder.rkmodel"
            self.assertEqual(meta["encoder"]["model_id"], "mlvc_rk3576_qp21_encoder")
            m, payloads = rkmdl1.unpack_model(enc.read_bytes())
            self.assertEqual((m["family"], m["role"], m["target"]),
                             ("mlvc", "encoder", "rk3576"))
            self.assertEqual([k for k, _, _ in payloads],
                             ["rknn", "pmf-gaussian", "pmf-bitest",
                              "qppatch", "qppatch"])
            _, dec_payloads = rkmdl1.unpack_model(
                (root / "mlvc_rk3576_qp21_decoder.rkmodel").read_bytes())
            self.assertEqual([k for k, _, _ in dec_payloads],
                             ["rknn", "pmf-gaussian", "pmf-bitest", "qppatch"])


if __name__ == "__main__":
    os.chdir(ROOT)
    unittest.main(verbosity=2)
