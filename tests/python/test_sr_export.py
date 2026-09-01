#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools" / "sr"
sys.path.insert(0, str(TOOLS))

import export_model
import verify_bundle

try:
    import numpy as np
    import build_calibration

    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

try:
    import onnx
    from onnx import TensorProto, helper

    HAS_ONNX = True
except ImportError:
    HAS_ONNX = False


class TestCheckpoint(unittest.TestCase):
    def test_prefers_ema_state(self) -> None:
        ema = {"weight": 1}
        self.assertIs(export_model._checkpoint_state({
            "state_dict": {"weight": 2}, "ema_state_dict": ema,
        }), ema)

    def test_plain_state(self) -> None:
        state = {"core.weight": object()}
        self.assertIs(export_model._checkpoint_state(state), state)


@unittest.skipUnless(HAS_NUMPY, "需要 numpy")
class TestCalibration(unittest.TestCase):
    def test_runtime_nv12_chroma_expansion(self) -> None:
        ycbcr = np.zeros((4, 4, 3), dtype=np.uint8)
        ycbcr[..., 0] = np.arange(16, dtype=np.uint8).reshape(4, 4)
        ycbcr[:2, :2, 1:] = (20, 30)
        ycbcr[:2, 2:, 1:] = (22, 32)
        ycbcr[2:, :2, 1:] = (24, 34)
        ycbcr[2:, 2:, 1:] = (26, 36)
        expanded = build_calibration.simulate_runtime_nv12(ycbcr)
        np.testing.assert_array_equal(expanded[..., 0], ycbcr[..., 0])
        np.testing.assert_array_equal(
            expanded[..., 1],
            [[20, 21, 22, 22], [22, 23, 24, 24],
             [24, 25, 26, 26], [24, 25, 26, 26]],
        )


@unittest.skipUnless(HAS_ONNX, "需要 onnx")
class TestOnnxContract(unittest.TestCase):
    def _model(self, path: Path, *, inputs: int = 1, out_channels: int = 108) -> None:
        phases = helper.make_tensor_value_info(
            "phases", TensorProto.FLOAT, [1, 12, 180, 320]
        )
        output = helper.make_tensor_value_info(
            "phase_residual", TensorProto.FLOAT, [1, out_channels, 180, 320]
        )
        graph_inputs = [phases]
        if inputs == 2:
            graph_inputs.append(helper.make_tensor_value_info(
                "codec_feature", TensorProto.FLOAT, [1, 96, 46, 80]
            ))
        shape = helper.make_tensor(
            "output_shape", TensorProto.INT64, [4], [1, out_channels, 180, 320]
        )
        value = helper.make_tensor("zero", TensorProto.FLOAT, [1], [0.0])
        node = helper.make_node(
            "ConstantOfShape", ["output_shape"], ["phase_residual"], value=value
        )
        graph = helper.make_graph([node], "phase", graph_inputs, [output], [shape])
        model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 18)])
        onnx.save(model, path)

    def test_accepts_single_input_phase_core(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sr.onnx"
            self._model(path)
            info = export_model.validate_onnx_contract(path, input_w=640, input_h=360)
            self.assertEqual(info["input"], [1, 12, 180, 320])
            self.assertEqual(info["output"], [1, 108, 180, 320])

    def test_rejects_codec_context(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sr.onnx"
            self._model(path, inputs=2)
            with self.assertRaises(export_model.ExportError):
                export_model.validate_onnx_contract(path, input_w=640, input_h=360)


class TestBundle(unittest.TestCase):
    def test_manifest_hashes_artifacts_and_source(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            bundle = root / "bundle"
            source.mkdir()
            bundle.mkdir()
            (source / "LICENSE").write_text("MIT\n", encoding="utf-8")
            (bundle / "phase_rlfn_sr_x3.onnx").write_bytes(b"onnx")
            manifest = export_model.write_bundle(
                source, bundle, source_commit="a" * 40, weight=None,
                input_w=640, input_h=360, target="rk3588", quantize=True,
            )
            data = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertFalse(data["model"]["codec_context"])
            self.assertEqual(data["model"]["input"]["shape"], [1, 12, 180, 320])
            self.assertIn("phase_rlfn_sr_x3.onnx", data["artifacts"])
            self.assertTrue((bundle / "LICENSE.rknn-super-resolution-MIT").is_file())

    def test_verify_rejects_tampered_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            bundle = root / "bundle"
            source.mkdir()
            bundle.mkdir()
            (source / "LICENSE").write_text("MIT\n", encoding="utf-8")
            onnx_path = bundle / "phase_rlfn_sr_x3.onnx"
            rknn_path = bundle / "phase_rlfn_sr_x3.rknn"
            onnx_path.write_bytes(b"onnx")
            rknn_path.write_bytes(b"rknn")
            export_model.write_bundle(
                source, bundle, source_commit="b" * 40, weight=None,
                input_w=640, input_h=360, target="rk3588", quantize=True,
                artifact_paths=[onnx_path, rknn_path],
            )
            verify_bundle.verify(bundle)
            rknn_path.write_bytes(b"tampered")
            with self.assertRaises(verify_bundle.BundleError):
                verify_bundle.verify(bundle)

    def test_finalize_portable_drops_onnx_and_refreshes_encrypted_rknn(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            bundle = root / "bundle"
            source.mkdir()
            bundle.mkdir()
            (source / "LICENSE").write_text("MIT\n", encoding="utf-8")
            onnx_path = bundle / "phase_rlfn_sr_x3.onnx"
            rknn_path = bundle / "phase_rlfn_sr_x3.rknn"
            onnx_path.write_bytes(b"onnx weights")
            rknn_path.write_bytes(b"plain rknn")
            export_model.write_bundle(
                source, bundle, source_commit="c" * 40, weight=None,
                input_w=640, input_h=360, target="rk3588", quantize=False,
                artifact_paths=[onnx_path, rknn_path],
            )
            onnx_path.unlink()
            rknn_path.write_bytes(b"RKVCENC1 encrypted rknn")
            manifest = verify_bundle.finalize_portable(bundle, encrypted=True)
            self.assertNotIn(onnx_path.name, manifest["artifacts"])
            self.assertTrue(manifest["distribution"]["encrypted"])
            verify_bundle.verify(bundle)

    def test_verify_rejects_malformed_model_shape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp)
            manifest = {
                "schema": 1,
                "model": {
                    "codec_context": False,
                    "input": {"shape": [12]},
                    "output": {"shape": [108]},
                },
                "artifacts": {},
            }
            (bundle / "sr_export_manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            with self.assertRaises(verify_bundle.BundleError):
                verify_bundle.verify(bundle)


if __name__ == "__main__":
    unittest.main()
