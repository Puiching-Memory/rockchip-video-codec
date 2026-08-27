#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""Export the open Phase-RLFN checkpoint to an RKVC ONNX/RKNN model bundle.

The upstream training environment and RKNN Toolkit intentionally use conflicting
Torch versions.  This adapter imports only the upstream model/export modules into
RKVC's pinned Python 3.12 environment, exports a static single-input ONNX graph,
then performs RKNN INT8 conversion and writes a reproducible bundle manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.metadata
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

UPSTREAM_URL = "https://github.com/Puiching-Memory/rknn-super-resolution.git"
UPSTREAM_REVISION = "15cdaad6c704e56018f52327f1a5a53200583e3e"
MODEL_STEM = "phase_rlfn_sr_x3"
INPUT_NAME = "phases"
OUTPUT_NAME = "phase_residual"
ONNX_OPSET = 17  # highest stable opset of RKVC's pinned torch 2.2 exporter


class ExportError(RuntimeError):
    """Model export or bundle validation failed."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package_version(name: str) -> str | None:
    try:
        return importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return None


def _run(command: Sequence[str], *, cwd: Path | None = None) -> None:
    proc = subprocess.run(command, cwd=cwd, text=True, check=False)
    if proc.returncode != 0:
        raise ExportError(f"命令失败 ({proc.returncode}): {' '.join(command)}")


def ensure_upstream(source_dir: Path, revision: str) -> str:
    """Create/update only the managed .build checkout, then verify its commit."""
    source_dir = source_dir.resolve()
    if not source_dir.exists():
        source_dir.parent.mkdir(parents=True, exist_ok=True)
        _run(["git", "clone", "--filter=blob:none", UPSTREAM_URL, str(source_dir)])
        _run(["git", "checkout", "--detach", revision], cwd=source_dir)
    if not (source_dir / "src/rknn_super_resolution/models/phase_rlfn_sr.py").is_file():
        raise ExportError(f"不是有效的 rknn-super-resolution 源码目录: {source_dir}")
    proc = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=source_dir,
        capture_output=True, text=True, check=False,
    )
    commit = proc.stdout.strip() if proc.returncode == 0 else "unknown"
    default_source = (Path(__file__).resolve().parents[2] /
                      ".build/deps/rknn-super-resolution").resolve()
    if revision and commit != revision and source_dir == default_source:
        _run(["git", "fetch", "--depth", "1", "origin", revision],
             cwd=source_dir)
        _run(["git", "checkout", "--detach", "FETCH_HEAD"], cwd=source_dir)
        proc = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=source_dir,
            capture_output=True, text=True, check=False,
        )
        commit = proc.stdout.strip() if proc.returncode == 0 else "unknown"
    if revision and commit != revision:
        raise ExportError(
            f"上游源码 commit={commit}，期望 {revision}。"
            "请使用默认托管目录，或显式传入与 --revision 一致的源码。"
        )
    return commit


def _import_upstream(source_dir: Path):
    src = str((source_dir / "src").resolve())
    if src not in sys.path:
        sys.path.insert(0, src)
    try:
        models = importlib.import_module("rknn_super_resolution.models")
        prep = importlib.import_module("rknn_super_resolution.deploy.export_prep")
    except ImportError as exc:
        raise ExportError(
            "无法导入上游模型。请在仓库根目录运行 `uv sync`，再用 "
            "`.venv/bin/python tools/sr/export_model.py ...`。"
        ) from exc
    return models.PhaseRLFNSR, prep


def _checkpoint_state(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise ExportError("checkpoint 必须是 state_dict 或包含 state_dict 的字典")
    for key in ("ema_state_dict", "state_dict"):
        value = raw.get(key)
        if isinstance(value, dict):
            return value
    return raw


def export_onnx(
    source_dir: Path,
    weight: Path,
    output: Path,
    *,
    input_w: int,
    input_h: int,
    from_qat: bool,
) -> Path:
    try:
        import torch
        import torch.nn as nn
    except ImportError as exc:
        raise ExportError("ONNX 导出需要项目 uv 环境中的 torch") from exc

    PhaseRLFNSR, prep = _import_upstream(source_dir)
    if input_w % 2 or input_h % 2:
        raise ExportError("Phase-RLFN 输入宽高必须为 2 的倍数")
    model = PhaseRLFNSR(scale=3, phase_factor=2)
    raw = torch.load(weight, map_location="cpu", weights_only=False)
    state = _checkpoint_state(raw)
    phases = torch.zeros(1, 12, input_h // 2, input_w // 2)
    if from_qat:
        # QAT state contains the same learnable parameter names/shapes as the
        # clean deploy model plus observer/fake-quant buffers.  Loading only the
        # exact deploy keys preserves learned weights while keeping unsupported
        # training ops out of ONNX.  The unused codec modules remain in the
        # module state but are pruned because forward_core receives no feature.
        deploy_state = model.state_dict()
        missing = [key for key in deploy_state if key not in state]
        mismatched = [
            key for key, value in deploy_state.items()
            if key in state and
            tuple(getattr(state[key], "shape", ())) != tuple(value.shape)
        ]
        if missing or mismatched:
            raise ExportError(
                f"QAT checkpoint 与 deploy graph 不匹配: missing={missing}, "
                f"shape_mismatch={mismatched}"
            )
        model.load_state_dict({key: state[key] for key in deploy_state}, strict=True)
        prep.prepare_float_for_export(model)
    else:
        model.load_state_dict(state, strict=True)
        prep.prepare_float_for_export(model)

    class SingleInputCore(nn.Module):
        def __init__(self, inner):
            super().__init__()
            self.inner = inner

        def forward(self, tensor):
            return self.inner.forward_core(tensor)

    wrapper = SingleInputCore(model).eval()
    example_inputs = (phases,)
    output.parent.mkdir(parents=True, exist_ok=True)
    # torch 2.2 legacy exporter is compatible with RKVC's pinned RKNN Toolkit;
    # the graph is static because rknn-toolkit2 compiles fixed tensor shapes.
    try:
        torch.onnx.export(
            wrapper,
            example_inputs,
            str(output),
            input_names=[INPUT_NAME],
            output_names=[OUTPUT_NAME],
            opset_version=ONNX_OPSET,
            do_constant_folding=True,
        )
    except Exception as exc:
        output.unlink(missing_ok=True)
        raise ExportError(f"ONNX 导出失败: {exc}") from exc
    validate_onnx_contract(output, input_w=input_w, input_h=input_h)
    return output


def _tensor_shape(value_info: Any) -> list[int | None]:
    result: list[int | None] = []
    for dim in value_info.type.tensor_type.shape.dim:
        result.append(int(dim.dim_value) if dim.HasField("dim_value") else None)
    return result


def validate_onnx_contract(path: Path, *, input_w: int, input_h: int) -> dict[str, Any]:
    try:
        import onnx
    except ImportError as exc:
        raise ExportError("验证 ONNX 需要 onnx") from exc
    model = onnx.load(str(path))
    onnx.checker.check_model(model)
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise ExportError("RKVC 仅接受 Phase-RLFN 单输入/单输出 fallback core")
    in_shape = _tensor_shape(model.graph.input[0])
    out_shape = _tensor_shape(model.graph.output[0])
    expected_in = [1, 12, input_h // 2, input_w // 2]
    expected_out = [1, 108, input_h // 2, input_w // 2]
    if in_shape != expected_in or out_shape != expected_out:
        raise ExportError(
            f"ONNX 契约不匹配: input={in_shape} output={out_shape}; "
            f"expected {expected_in} -> {expected_out}"
        )
    return {"input": in_shape, "output": out_shape, "opset": model.opset_import[0].version}


def convert_rknn(
    onnx_path: Path,
    output: Path,
    *,
    target: str,
    calibration_list: Path | None,
    quantize: bool,
    encrypt: bool,
    crypt_level: int,
    verbose: bool,
    input_w: int,
    input_h: int,
) -> tuple[Path, Path | None]:
    try:
        from rknn.api import RKNN
    except ImportError as exc:
        raise ExportError("RKNN 转换需要项目 uv 环境中的 rknn-toolkit2") from exc
    if quantize and (not calibration_list or not calibration_list.is_file()):
        raise ExportError("INT8 转换需要 --calibration-list")
    output.parent.mkdir(parents=True, exist_ok=True)
    rknn = RKNN(verbose=verbose)
    encrypted: Path | None = None
    try:
        ret = rknn.config(
            mean_values=[[0] * 12],
            std_values=[[1] * 12],
            target_platform=target,
            quantized_algorithm="kl_divergence",
            quantized_method="channel",
            optimization_level=3,
        )
        if ret not in (None, 0):
            raise ExportError(f"rknn.config 失败: {ret}")
        ret = rknn.load_onnx(
            model=str(onnx_path),
            # 显式钉住输入名与逻辑形状（NCHW 语义 1×12×H/2×W/2）。
            # 注意：工具链仍会把宿主输入属性归一为 NHWC（dims
            # 1×H/2×W/2×12，输出属性保持 NCHW），rknn-toolkit2 无法通过
            # 该参数改变记法；运行时契约因此是「输入 NHWC 交错 / 输出
            # NCHW 平面」，见 docs/sr-model-yuv-spec.md。
            inputs=[INPUT_NAME],
            input_size_list=[[1, 12, input_h // 2, input_w // 2]],
        )
        if ret != 0:
            raise ExportError(f"rknn.load_onnx 失败: {ret}")
        ret = rknn.build(
            do_quantization=quantize,
            dataset=str(calibration_list.resolve()) if quantize else None,
        )
        if ret != 0:
            raise ExportError(f"rknn.build 失败: {ret}")
        ret = rknn.export_rknn(str(output))
        if ret != 0:
            raise ExportError(f"rknn.export_rknn 失败: {ret}")
        if encrypt:
            encrypted = output.with_suffix(".crypt.rknn")
            ret = rknn.export_encrypted_rknn_model(
                str(output), output_model=str(encrypted), crypt_level=crypt_level
            )
            if ret != 0:
                raise ExportError(f"RKNN 加密失败: {ret}")
    except ExportError:
        raise
    except Exception as exc:
        raise ExportError(f"rknn-toolkit2 转换异常: {exc}") from exc
    finally:
        rknn.release()
    return output, encrypted


def write_bundle(
    source_dir: Path,
    output_dir: Path,
    *,
    source_commit: str,
    weight: Path | None,
    input_w: int,
    input_h: int,
    target: str,
    quantize: bool,
    artifact_paths: Sequence[Path] | None = None,
) -> Path:
    artifacts: dict[str, dict[str, Any]] = {}
    candidates = artifact_paths or sorted(output_dir.glob(f"{MODEL_STEM}*"))
    for path in candidates:
        if path.is_file():
            artifacts[path.name] = {"bytes": path.stat().st_size, "sha256": sha256_file(path)}
    if not artifacts:
        raise ExportError(f"bundle 中没有模型产物: {output_dir}")

    license_src = source_dir / "LICENSE"
    if not license_src.is_file():
        raise ExportError(f"上游 LICENSE 不存在: {license_src}")
    license_dst = output_dir / "LICENSE.rknn-super-resolution-MIT"
    shutil.copy2(license_src, license_dst)
    source_note = output_dir / "SOURCE.md"
    source_note.write_text(
        "# rknn-super-resolution source\n\n"
        f"- Repository: {UPSTREAM_URL}\n"
        f"- Commit: `{source_commit}`\n"
        "- Export mode: single-input fallback core (`--no-codec-context`)\n"
        "- License: MIT (see `LICENSE.rknn-super-resolution-MIT`)\n",
        encoding="utf-8",
    )
    for path in (license_dst, source_note):
        artifacts[path.name] = {"bytes": path.stat().st_size, "sha256": sha256_file(path)}
    manifest = {
        "schema": 1,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "source": {"repository": UPSTREAM_URL, "commit": source_commit},
        "model": {
            "architecture": "Phase-RLFN",
            "scale": 3,
            "colorspace": "BT.709 full-range YCbCr444",
            "codec_context": False,
            "input": {"name": INPUT_NAME, "shape": [1, 12, input_h // 2, input_w // 2]},
            "output": {"name": OUTPUT_NAME, "shape": [1, 108, input_h // 2, input_w // 2]},
            "runtime_preprocess": "NV12 chroma expand + PixelUnshuffle(2)",
            "runtime_postprocess": "RGA bicubic + PixelShuffle(6) residual + NV12 chroma average",
        },
        "rknn": {"target": target, "quantized": quantize},
        "toolchain": {
            "python": sys.version.split()[0],
            "torch": package_version("torch"),
            "onnx": package_version("onnx"),
            "rknn_toolkit2": package_version("rknn-toolkit2"),
        },
        "checkpoint": (
            {"filename": weight.name, "sha256": sha256_file(weight)} if weight else None
        ),
        "artifacts": artifacts,
    }
    manifest_path = output_dir / "sr_export_manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
    return manifest_path


def build_parser() -> argparse.ArgumentParser:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weight", type=Path, help="上游 best_ema.pth / float checkpoint")
    parser.add_argument("--onnx", type=Path, help="复用已有单输入 ONNX（跳过 checkpoint 导出）")
    parser.add_argument("--calibration-list", type=Path, help="RKNN PTQ .npy 样本列表")
    parser.add_argument("--source-dir", type=Path,
                        default=root / ".build/deps/rknn-super-resolution")
    parser.add_argument("--revision", default=UPSTREAM_REVISION)
    parser.add_argument("--output-dir", type=Path, default=root / "models/rkvc-sr")
    parser.add_argument("--target", default="rk3588",
                        choices=("rk3588", "rk3576", "rv1126b"))
    parser.add_argument("--input-width", type=int, default=640)
    parser.add_argument("--input-height", type=int, default=360)
    parser.add_argument("--from-qat", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--quantize", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--encrypt", action=argparse.BooleanOptionalAction, default=False,
                        help="可选 RKNN 加密；要求 wheel/主机提供 rknn_crypt_tool")
    parser.add_argument("--crypt-level", type=int, choices=(1, 2, 3), default=2)
    parser.add_argument("--onnx-only", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.weight and args.onnx:
        raise ExportError("--weight 与 --onnx 只能二选一")
    if not args.weight and not args.onnx:
        raise ExportError("必须提供 --weight 或 --onnx")
    if args.weight and not args.weight.is_file():
        raise ExportError(f"checkpoint 不存在: {args.weight}")

    source_dir = args.source_dir.resolve()
    commit = ensure_upstream(source_dir, args.revision)
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = output_dir / f"{MODEL_STEM}.onnx"
    if args.onnx:
        validate_onnx_contract(args.onnx, input_w=args.input_width, input_h=args.input_height)
        shutil.copy2(args.onnx, onnx_path)
    else:
        export_onnx(
            source_dir, args.weight, onnx_path,
            input_w=args.input_width, input_h=args.input_height,
            from_qat=args.from_qat,
        )

    artifacts = [onnx_path]
    if not args.onnx_only:
        plain_rknn, encrypted_rknn = convert_rknn(
            onnx_path, output_dir / f"{MODEL_STEM}.rknn",
            target=args.target, calibration_list=args.calibration_list,
            quantize=args.quantize, encrypt=args.encrypt,
            crypt_level=args.crypt_level, verbose=args.verbose,
            input_w=args.input_width, input_h=args.input_height,
        )
        artifacts.append(plain_rknn)
        if encrypted_rknn:
            artifacts.append(encrypted_rknn)
    manifest = write_bundle(
        source_dir, output_dir, source_commit=commit, weight=args.weight,
        input_w=args.input_width, input_h=args.input_height,
        target=args.target, quantize=args.quantize,
        artifact_paths=artifacts,
    )
    print(f"SR bundle: {output_dir}")
    print(f"Manifest: {manifest}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ExportError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
