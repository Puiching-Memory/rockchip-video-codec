#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""MLVC → RKNN 导出：PMF1 转换（stdlib）与可选 ONNX 图重写。"""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "mlvc"
EXPORT = TOOLS / "export_rknn.py"
sys.path.insert(0, str(TOOLS))

import pmf  # noqa: E402
import qppatch  # noqa: E402
import rknn_convert  # noqa: E402
import export_rknn  # noqa: E402

try:
    import onnx  # type: ignore  # noqa: F401

    HAS_ONNX = True
except ImportError:
    HAS_ONNX = False

GAUSSIAN_JSON = {
    "pmf_lengths": [2, 3],
    "pmf_offsets": [0, 2],
    "pmf_table": [1, 2, 3, 4, 5],
    "scale_min": 0.11,
    "scale_max": 16.0,
    "scale_levels": 128,
    "index_space": True,
}

BITEST_JSON = {
    "pmf_lengths": [4, 5, 6, 7],
    "pmf_offsets": [0, 4, 9, 15],
    "pmf_table": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22],
    "qp_num": 2,
    "channels": 2,
}


class TestPmf1(unittest.TestCase):
    def test_gaussian_roundtrip(self) -> None:
        blob = pmf.json_to_pmf1(GAUSSIAN_JSON, kind="gaussian")
        self.assertTrue(blob.startswith(b"PMF1"))
        back = pmf.pmf1_to_dict(blob)
        self.assertEqual(back["kind"], "gaussian")
        self.assertEqual(back["pmf_lengths"], GAUSSIAN_JSON["pmf_lengths"])
        self.assertEqual(back["pmf_offsets"], GAUSSIAN_JSON["pmf_offsets"])
        self.assertEqual(back["pmf_table"], GAUSSIAN_JSON["pmf_table"])
        self.assertEqual(back["scale_levels"], 128)
        self.assertTrue(back["index_space"])
        self.assertAlmostEqual(back["scale_min"], 0.11)
        self.assertAlmostEqual(back["scale_max"], 16.0)

    def test_bitest_roundtrip(self) -> None:
        blob = pmf.json_to_pmf1(BITEST_JSON, kind="bitest")
        back = pmf.pmf1_to_dict(blob)
        self.assertEqual(back["kind"], "bitest")
        self.assertEqual(back["qp_num"], 2)
        self.assertEqual(back["channels"], 2)
        self.assertEqual(back["pmf_lengths"], BITEST_JSON["pmf_lengths"])

    def test_detect_kind(self) -> None:
        self.assertEqual(pmf.detect_kind(GAUSSIAN_JSON), "gaussian")
        self.assertEqual(pmf.detect_kind(BITEST_JSON), "bitest")

    def test_bitest_size_mismatch(self) -> None:
        bad = dict(BITEST_JSON)
        bad["qp_num"] = 9
        with self.assertRaises(pmf.PmfError):
            pmf.json_to_pmf1(bad, kind="bitest")

    def test_missing_fields(self) -> None:
        with self.assertRaises(pmf.PmfError):
            pmf.json_to_pmf1({"scale_min": 1}, kind="gaussian")

    def test_bad_magic(self) -> None:
        with self.assertRaises(pmf.PmfError):
            pmf.pmf1_to_dict(b"XXXX" + b"\x00" * 20)

    def test_truncated(self) -> None:
        blob = pmf.json_to_pmf1(GAUSSIAN_JSON)
        with self.assertRaises(pmf.PmfError):
            pmf.pmf1_to_dict(blob[:20])

    def test_convert_json_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "gaussian_pmf.json"
            dst = Path(tmp) / "gaussian.bin"
            src.write_text(json.dumps(GAUSSIAN_JSON), encoding="utf-8")
            meta = pmf.convert_json_file(src, dst)
            self.assertTrue(dst.is_file())
            self.assertEqual(meta["bytes"], dst.stat().st_size)
            self.assertTrue(meta["index_space"])


class TestCli(unittest.TestCase):
    def test_default_variant_bundle_dirs(self) -> None:
        self.assertEqual(export_rknn.default_output_dir("dmc61sbr_reglu"), Path("models/mlvc"))
        self.assertEqual(export_rknn.default_output_dir("dmc61sbr_reglu_s"), Path("models/mlvc-s"))

    def test_help_without_args(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(EXPORT)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 2)
        self.assertIn("usage", (proc.stdout + proc.stderr).lower())

    def test_pmf_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src_dir = Path(tmp) / "in"
            out_dir = Path(tmp) / "out"
            src_dir.mkdir()
            (src_dir / "gaussian_pmf.json").write_text(json.dumps(GAUSSIAN_JSON), encoding="utf-8")
            (src_dir / "bit_estimator_pmf.json").write_text(json.dumps(BITEST_JSON), encoding="utf-8")
            proc = subprocess.run(
                [
                    sys.executable,
                    str(EXPORT),
                    "--onnx-dir",
                    str(src_dir),
                    "--out-dir",
                    str(out_dir),
                    "--pmf-only",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            gbin = out_dir / "gaussian.bin"
            bbin = out_dir / "bitest.bin"
            self.assertTrue(gbin.is_file())
            self.assertTrue(bbin.is_file())
            g = pmf.pmf1_to_dict(gbin.read_bytes())
            b = pmf.pmf1_to_dict(bbin.read_bytes())
            self.assertEqual(g["kind"], "gaussian")
            self.assertEqual(b["kind"], "bitest")
            self.assertTrue(g["index_space"])

    def test_rknn_missing_is_explicit(self) -> None:
        if rknn_convert.has_local_rknn():
            self.skipTest("本机已安装 rknn-toolkit2")
        with self.assertRaises(rknn_convert.RknnConvertError) as ctx:
            rknn_convert.require_rknn()
        self.assertIn("rknn-toolkit2", str(ctx.exception))

    def test_rknn_conversion_is_strict_fp16_without_quantization(self) -> None:
        calls: dict[str, object] = {}

        class FakeRKNN:
            def __init__(self, *, verbose: bool) -> None:
                calls["verbose"] = verbose

            def config(self, **kwargs: object) -> int:
                calls["config"] = kwargs
                return 0

            def load_onnx(self, **kwargs: object) -> int:
                calls["load"] = kwargs
                return 0

            def build(self, **kwargs: object) -> int:
                calls["build"] = kwargs
                return 0

            def export_rknn(self, path: str) -> int:
                Path(path).write_bytes(b"RKNN")
                return 0

            def release(self) -> None:
                calls["released"] = True

        original = rknn_convert.require_rknn
        rknn_convert.require_rknn = lambda: FakeRKNN
        try:
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / "model.onnx"
                dst = Path(tmp) / "model.rknn"
                src.write_bytes(b"ONNX")
                self.assertEqual(
                    rknn_convert.convert_onnx_to_rknn(src, dst), dst
                )
        finally:
            rknn_convert.require_rknn = original

        self.assertEqual(calls["config"]["float_dtype"], "float16")  # type: ignore[index]
        self.assertEqual(calls["build"], {"do_quantization": False})
        self.assertTrue(calls["released"])

    def test_rknn_conversion_never_drops_fp16_for_old_toolkit(self) -> None:
        configs: list[dict[str, object]] = []

        class FakeOldRKNN:
            def __init__(self, *, verbose: bool) -> None:
                del verbose

            def config(self, **kwargs: object) -> int:
                configs.append(kwargs)
                if "optimization_level" in kwargs:
                    raise TypeError("old optimization API")
                if "float_dtype" in kwargs:
                    raise TypeError("no float_dtype")
                return 0

            def release(self) -> None:
                pass

        original = rknn_convert.require_rknn
        rknn_convert.require_rknn = lambda: FakeOldRKNN
        try:
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / "model.onnx"
                src.write_bytes(b"ONNX")
                with self.assertRaises(rknn_convert.RknnConvertError) as ctx:
                    rknn_convert.convert_onnx_to_rknn(
                        src, Path(tmp) / "model.rknn"
                    )
        finally:
            rknn_convert.require_rknn = original

        self.assertIn("float_dtype=float16", str(ctx.exception))
        self.assertEqual(len(configs), 2)
        self.assertTrue(all(c.get("float_dtype") == "float16" for c in configs))


@unittest.skipUnless(HAS_ONNX, "需要 pip install onnx（图重写测试）")
class TestOnnxRewrite(unittest.TestCase):
    def _tiny_encoder(self, path: Path) -> None:
        import numpy as np
        from onnx import TensorProto, helper, numpy_helper

        x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 3, 8, 8])
        ref = helper.make_tensor_value_info("ref_feature", TensorProto.FLOAT, [1, 4, 2, 2])
        q = helper.make_tensor_value_info("q_index_shifted", TensorProto.INT32, [1])
        y0 = helper.make_tensor_value_info("y_raw_0", TensorProto.FLOAT, [1, 12, 4, 4])
        y1 = helper.make_tensor_value_info("y_raw_1", TensorProto.FLOAT, [1, 12, 4, 4])
        feat = helper.make_tensor_value_info("feature", TensorProto.FLOAT, [1, 4, 2, 2])
        z = helper.make_tensor_value_info("z_raw", TensorProto.FLOAT, [1, 4, 2, 2])

        zero = numpy_helper.from_array(np.array(0.0, dtype=np.float32), name="zero")
        two = numpy_helper.from_array(np.array(2.0, dtype=np.float32), name="two")
        qtab = numpy_helper.from_array(np.zeros((64,), dtype=np.float32), name="q_table")

        nodes = [
            helper.make_node("SpaceToDepth", ["x"], ["std"], blocksize=2, name="unshuffle"),
            helper.make_node("Max", ["std", "zero"], ["clipped"], name="mx"),
            helper.make_node("Div", ["clipped", "two"], ["y_raw_0"], name="dv"),
            helper.make_node("Identity", ["y_raw_0"], ["y_raw_1"], name="y1"),
            helper.make_node("Identity", ["ref_feature"], ["feature"], name="feat"),
            helper.make_node("Identity", ["ref_feature"], ["z_raw"], name="z"),
            helper.make_node("Gather", ["q_table", "q_index_shifted"], ["q_emb"], axis=0, name="qgather"),
        ]
        graph = helper.make_graph(
            nodes,
            "tiny_enc",
            [x, q, ref],
            [y1, feat, z, y0],
            [zero, two, qtab],
        )
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        model.ir_version = 8
        onnx.save(model, str(path))

    def test_prepare_folds_and_rewrites(self) -> None:
        from onnx_rewrite import inspect_onnx, prepare_onnx, validate_runtime_io

        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "MLVCEncoder.onnx"
            dst = Path(tmp) / "prepared.onnx"
            self._tiny_encoder(src)
            before = inspect_onnx(src)
            self.assertEqual(len(before.inputs), 3)
            info, report = prepare_onnx(src, dst, qp=21, rewrite=True, fold=True)
            self.assertIn("q_index_shifted", report.folded_inputs)
            self.assertEqual(report.space_to_depth, 1)
            self.assertEqual(report.max_to_clip, 1)
            self.assertEqual(report.div_to_mul, 1)
            self.assertEqual([t.name for t in info.inputs], ["x", "ref_feature"])
            self.assertEqual(
                [t.name for t in info.outputs],
                ["feature", "z_raw", "y_raw_0", "y_raw_1"],
            )
            self.assertEqual(validate_runtime_io(info, part="encoder"), [])

            loaded = onnx.load(str(dst))
            ops = [n.op_type for n in loaded.graph.node]
            self.assertNotIn("SpaceToDepth", ops)
            self.assertNotIn("Max", ops)
            self.assertNotIn("Div", ops)
            self.assertIn("Clip", ops)
            self.assertIn("Mul", ops)
            self.assertIn("Transpose", ops)
            perm = None
            for node in loaded.graph.node:
                if node.op_type == "Transpose":
                    for attr in node.attribute:
                        if attr.name == "perm":
                            perm = list(attr.ints)
            self.assertEqual(perm, [0, 3, 5, 1, 2, 4])
            in_names = [i.name for i in loaded.graph.input]
            self.assertNotIn("q_index_shifted", in_names)

    def test_extract_leading_space_to_depth(self) -> None:
        from onnx_extract import extract_file
        from onnx_rewrite import inspect_onnx, prepare_onnx

        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "enc.onnx"
            prepared = Path(tmp) / "prepared.onnx"
            extracted = Path(tmp) / "extracted.onnx"
            self._tiny_encoder(src)
            prepare_onnx(src, prepared, qp=21, rewrite=False, fold=True)
            report = extract_file(prepared, extracted, "space_to_depth")
            self.assertEqual(report.blocksize, 2)
            self.assertEqual(report.in_shape, [1, 3, 8, 8])
            self.assertEqual(report.out_shape, [1, 12, 4, 4])
            info = inspect_onnx(extracted)
            x = next(t for t in info.inputs if t.name == "x")
            self.assertEqual(x.shape, [1, 12, 4, 4])
            ops = [n.op_type for n in onnx.load(str(extracted)).graph.node]
            self.assertNotIn("SpaceToDepth", ops)

    def test_extract_does_not_drop_other_unnamed_nodes(self) -> None:
        from onnx_extract import extract_file

        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "enc.onnx"
            self._tiny_encoder(src)
            loaded = onnx.load(str(src))
            for node in loaded.graph.node:
                node.name = ""
            onnx.save(loaded, str(src))
            dst = Path(tmp) / "ex.onnx"
            extract_file(src, dst, "space_to_depth")
            ops = [n.op_type for n in onnx.load(str(dst)).graph.node]
            self.assertNotIn("SpaceToDepth", ops)
            self.assertIn("Max", ops)
            self.assertIn("Div", ops)
            self.assertIn("Identity", ops)

    def _tiny_decoder(self, path: Path) -> None:
        import numpy as np
        from onnx import TensorProto, helper, numpy_helper

        z = helper.make_tensor_value_info("z_raw", TensorProto.FLOAT, [1, 12, 2, 2])
        y0 = helper.make_tensor_value_info("y_raw_0", TensorProto.FLOAT, [1, 4, 2, 2])
        y1 = helper.make_tensor_value_info("y_raw_1", TensorProto.FLOAT, [1, 4, 2, 2])
        ref = helper.make_tensor_value_info("ref_feature", TensorProto.FLOAT, [1, 4, 2, 2])
        x_hat = helper.make_tensor_value_info("x_hat", TensorProto.FLOAT, [1, 3, 4, 4])
        feat = helper.make_tensor_value_info("feature", TensorProto.FLOAT, [1, 4, 2, 2])
        zero = numpy_helper.from_array(np.array(0.0, dtype=np.float32), name="zero")
        one = numpy_helper.from_array(np.array(1.0, dtype=np.float32), name="one")
        nodes = [
            helper.make_node("DepthToSpace", ["z_raw"], ["shuf"], blocksize=2, name="d2s"),
            helper.make_node("Clip", ["shuf", "zero", "one"], ["x_hat"], name="clip_x"),
            helper.make_node("Identity", ["ref_feature"], ["feature"], name="feat"),
            helper.make_node("Identity", ["y_raw_0"], ["y0_keep"], name="keep0"),
            helper.make_node("Identity", ["y_raw_1"], ["y1_keep"], name="keep1"),
        ]
        graph = helper.make_graph(
            nodes, "tiny_dec", [z, y0, y1, ref], [x_hat, feat], [zero, one]
        )
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
        model.ir_version = 8
        onnx.save(model, str(path))

    def test_extract_trailing_depth_to_space(self) -> None:
        from onnx_extract import extract_file
        from onnx_rewrite import inspect_onnx

        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "dec.onnx"
            dst = Path(tmp) / "extracted.onnx"
            self._tiny_decoder(src)
            report = extract_file(src, dst, "depth_to_space")
            self.assertEqual(report.blocksize, 2)
            self.assertEqual(report.in_shape, [1, 12, 2, 2])
            self.assertEqual(report.clip_lo, 0.0)
            self.assertEqual(report.clip_hi, 1.0)
            self.assertEqual(report.mode, "DCR")
            info = inspect_onnx(dst)
            x = next(t for t in info.outputs if t.name == "x_hat")
            self.assertEqual(x.shape, [1, 12, 2, 2])
            ops = [n.op_type for n in onnx.load(str(dst)).graph.node]
            self.assertNotIn("DepthToSpace", ops)
            self.assertNotIn("Clip", ops)

    def test_skip_rknn_cli_writes_onnx(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src_dir = Path(tmp) / "in"
            out_dir = Path(tmp) / "out"
            src_dir.mkdir()
            self._tiny_encoder(src_dir / "MLVCEncoder.onnx")
            proc = subprocess.run(
                [
                    sys.executable,
                    str(EXPORT),
                    "--onnx-dir",
                    str(src_dir),
                    "--out-dir",
                    str(out_dir),
                    "--skip-rknn",
                    "--qp",
                    "21",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            onnx_out = out_dir / "MLVCEncoder_rk3588.onnx"
            self.assertTrue(onnx_out.is_file(), proc.stdout)
            manifest = out_dir / "mlvc_rknn_export_manifest.json"
            self.assertTrue(manifest.is_file())
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(manifest_data["rknn_export"]["precision"], "fp16")
            self.assertFalse(manifest_data["rknn_export"]["do_quantization"])
            self.assertTrue(manifest_data["rknn_export"]["skipped"])
            self.assertEqual(
                manifest_data["models"]["encoder"]["qps"]["21"]["rknn_precision"],
                "fp16",
            )
            self.assertFalse(
                manifest_data["models"]["encoder"]["qps"]["21"]["quantized"]
            )


class TestQppatch(unittest.TestCase):
    def test_apply_roundtrip(self) -> None:
        base = bytes(range(256)) * 4
        target = bytearray(base)
        target[10:16] = b"\xaa" * 6
        target[100] = 0xBB
        target[400:450] = bytes(range(50))
        blob = qppatch.encode(bytes(base), bytes(target), 25)
        restored = qppatch.apply(base, blob, expected_qp=25)
        self.assertEqual(restored, bytes(target))
        meta = qppatch.decode_header(blob)
        self.assertEqual(meta["qp"], 25)
        self.assertEqual(meta["base_size"], len(base))
        self.assertGreater(meta["num_ranges"], 0)

    def test_empty_patch(self) -> None:
        base = b"\x11" * 64
        blob = qppatch.encode(base, base, 21)
        self.assertEqual(qppatch.decode_header(blob)["num_ranges"], 0)
        self.assertEqual(qppatch.apply(base, blob, expected_qp=21), base)
        self.assertEqual(struct.calcsize(qppatch.HEADER_FMT), qppatch.HEADER_SIZE)

    def test_coalesce_gap(self) -> None:
        base = bytearray(b"\x00" * 32)
        target = bytearray(base)
        target[0] = 1
        target[2] = 2
        self.assertEqual(len(qppatch.diff_ranges(bytes(base), bytes(target), gap=64)), 1)
        self.assertEqual(len(qppatch.diff_ranges(bytes(base), bytes(target), gap=0)), 2)

    def test_size_mismatch(self) -> None:
        with self.assertRaises(qppatch.QppatchError):
            qppatch.encode(b"abc", b"abcd", 21)

    def test_wrong_qp_and_crc(self) -> None:
        base = b"\x22" * 32
        target = bytearray(base)
        target[0] = 0x33
        blob = qppatch.encode(base, bytes(target), 29)
        with self.assertRaises(qppatch.QppatchError):
            qppatch.apply(base, blob, expected_qp=21)
        other = b"\x44" * 32
        with self.assertRaises(qppatch.QppatchError):
            qppatch.apply(other, blob, expected_qp=29)

    def test_generate_from_qp_dir_and_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            models = root / "rk3588_qp_models"
            for qp, marker in ((21, 0x21), (25, 0x25)):
                d = models / f"qp{qp}"
                d.mkdir(parents=True)
                enc = bytearray(b"RKNN" + b"\x00" * 200)
                dec = bytearray(b"RKNN" + b"\x01" * 200)
                enc[40] = marker
                dec[80] = marker
                (d / "MLVCEncoder_rk3588.rknn").write_bytes(bytes(enc))
                (d / "MLVCDecoder_rk3588.rknn").write_bytes(bytes(dec))

            out = root / "qp_patches"
            summary = qppatch.generate_from_qp_dir(models, out, base_qp=21)
            self.assertEqual(summary["base_qp"], 21)
            self.assertTrue((out / "enc_qp21.qppatch").is_file())
            self.assertTrue((out / "enc_qp25.qppatch").is_file())
            self.assertTrue((out / "dec_qp25.qppatch").is_file())
            self.assertTrue((out / "qppatch_manifest.json").is_file())

            base_enc = (models / "qp21" / "MLVCEncoder_rk3588.rknn").read_bytes()
            tgt_enc = (models / "qp25" / "MLVCEncoder_rk3588.rknn").read_bytes()
            restored = qppatch.apply(
                base_enc, (out / "enc_qp25.qppatch").read_bytes(), expected_qp=25
            )
            self.assertEqual(restored, tgt_enc)

            cli_out = root / "cli_patches"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "make_qp_patches.py"),
                    "--models-dir",
                    str(models),
                    "--base-qp",
                    "21",
                    "--out-dir",
                    str(cli_out),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertTrue((cli_out / "enc_qp25.qppatch").is_file())

    def test_generate_rejects_different_sizes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            models = Path(tmp) / "models"
            (models / "qp21").mkdir(parents=True)
            (models / "qp25").mkdir()
            (models / "qp21" / "MLVCEncoder_rk3588.rknn").write_bytes(b"A" * 100)
            (models / "qp25" / "MLVCEncoder_rk3588.rknn").write_bytes(b"B" * 80)
            with self.assertRaises(qppatch.QppatchError):
                qppatch.generate_from_qp_dir(models, Path(tmp) / "out", base_qp=21, parts=("enc",))


class TestOnnxExport(unittest.TestCase):
    def test_qualcomm_rejected(self) -> None:
        import export_onnx

        with self.assertRaises(export_onnx.OnnxExportError):
            export_onnx.export_onnx(target_device="qualcomm")

    def test_dummy_yuv_i420_size(self) -> None:
        import export_onnx

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.yuv"
            export_onnx.write_dummy_yuv420(path, 640, 360, frames=2)
            self.assertEqual(path.stat().st_size, 640 * 360 * 3 // 2 * 2)

    def test_mlvc_s_checkpoint_auto_download(self) -> None:
        import export_onnx

        # 已就绪的本地副本（SHA-256 命中）直接复用，不触发下载
        with tempfile.TemporaryDirectory() as tmp:
            data_dir = Path(tmp)
            dest = data_dir / "pretrained" / "mlvc-s-psnr-v1.ckpt"
            dest.parent.mkdir(parents=True)
            dest.write_bytes(b"mlvc-s-weights")
            real_sha = export_onnx.CKPT_S_SHA256
            real_dl = export_onnx.download_file
            export_onnx.CKPT_S_SHA256 = export_onnx.sha256_file(dest)
            export_onnx.download_file = lambda *a, **k: self.fail("不应下载")
            try:
                got = export_onnx.ensure_checkpoint(
                    data_dir, None, export_onnx.DEFAULT_S_MODEL_VERSION
                )
            finally:
                export_onnx.CKPT_S_SHA256 = real_sha
                export_onnx.download_file = real_dl
            self.assertEqual(got, dest)

        # 下载失败时报错并提示手动放置 / --weights-path
        with tempfile.TemporaryDirectory() as tmp:
            data_dir = Path(tmp)

            def boom(*args, **kwargs):
                raise OSError("network down")

            real_dl = export_onnx.download_file
            export_onnx.download_file = boom
            try:
                with self.assertRaisesRegex(export_onnx.OnnxExportError, "--weights-path"):
                    export_onnx.ensure_checkpoint(
                        data_dir, None, export_onnx.DEFAULT_S_MODEL_VERSION
                    )
            finally:
                export_onnx.download_file = real_dl

    def test_find_exported_onnx(self) -> None:
        import export_onnx

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            d = root / "dmc61sbr_reglu-mlvc-psnr-v1" / "onnx-generic" / "640x368"
            d.mkdir(parents=True)
            (d / "MLVCEncoder.onnx").write_bytes(b"x")
            (d / "MLVCDecoder.onnx").write_bytes(b"y")
            (d / "gaussian_pmf.json").write_text("{}")
            self.assertEqual(export_onnx.find_exported_onnx(root), d)

    def test_from_mlvc_help(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(EXPORT), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("--from-mlvc", proc.stdout)
        self.assertIn("--onnx-frame-count", proc.stdout)

    def test_patch_mlvc_checkout(self) -> None:
        import export_onnx

        fake_pyproject = """
[project]
dependencies = [
 "torch==2.10.0",
 "coremltools",
]
[tool.uv]
environments = [
    "sys_platform == 'linux' and platform_machine == 'x86_64'",
    "sys_platform == 'darwin' and platform_machine == 'arm64'",
]
[tool.uv.sources]
torch = [
    { index = "pytorch-cu128", marker = "sys_platform == 'win32' and platform_machine == 'AMD64'" },
    { index = "pytorch-cpu", marker = "sys_platform == 'win32' and platform_machine != 'AMD64'" },
]
[[tool.uv.index]]
name = "pytorch-cu128"
url = "https://download.pytorch.org/whl/cu128"
explicit = true
[[tool.uv.index]]
name = "pytorch-cpu"
url = "https://download.pytorch.org/whl/cpu"
"""
        fake_utils = '''
def download_test_data(path: Path | str, base_path: Path | str = DEFAULT_TEST_DATA_DIR) -> Path:
    if Path(path).is_absolute():
        return Path(path)

    account = get_required_env("AZURE_TEST_DATA_STORAGE_ACCOUNT")

def download_job_outputs(path: Path | str, base_path: Path | str = DEFAULT_JOB_OUTPUTS_DIR) -> Path:
    if Path(path).is_absolute():
        return Path(path)

    account = get_required_env("AZURE_JOB_OUTPUTS_STORAGE_ACCOUNT")
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cfg = root / "video" / "conversion" / "_full_model"
            cfg.mkdir(parents=True)
            (root / "pyproject.toml").write_text(fake_pyproject, encoding="utf-8")
            (cfg / "model_configs_example.yaml").write_text("dmc61sbr_reglu: {}\n", encoding="utf-8")
            (root / "video" / "conversion" / "utils.py").write_text(fake_utils, encoding="utf-8")
            (root / "video" / "conversion" / "_model_bundler.py").write_text(
                "import onnx\nimport coremltools as ct\n", encoding="utf-8"
            )
            export_onnx.patch_mlvc_checkout(root)
            pyproject = (root / "pyproject.toml").read_text(encoding="utf-8")
            self.assertIn(export_onnx._uv_environment_marker(), pyproject)
            self.assertIn("coremltools; sys_platform == 'darwin'", pyproject)
            self.assertIn('torch = [\n    { index = "pytorch-cpu" },\n]', pyproject)
            self.assertNotIn("pytorch-cu128", pyproject)
            self.assertTrue((cfg / "model_configs.yaml").is_file())
            utils = (root / "video" / "conversion" / "utils.py").read_text(encoding="utf-8")
            self.assertEqual(utils.count("if local.is_file():"), 2)
            self.assertIn("AZURE_TEST_DATA_STORAGE_ACCOUNT", utils)
            self.assertIn("AZURE_JOB_OUTPUTS_STORAGE_ACCOUNT", utils)
            self.assertIn(
                "except ImportError:",
                (root / "video" / "conversion" / "_model_bundler.py").read_text(encoding="utf-8"),
            )


if __name__ == "__main__":
    os.chdir(ROOT)
    unittest.main(verbosity=2)
