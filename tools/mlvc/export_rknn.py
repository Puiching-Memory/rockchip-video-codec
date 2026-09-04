#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""把 Microsoft MLVC 导出的 ONNX / PMF JSON 转成 rkvc 使用的 ``.rknn`` + ``PMF1``。

一条龙（浅克隆 microsoft/mlvc 并跑 convert.py）：

    python export_rknn.py --from-mlvc --out-dir models/mlvc --qp 21

或消费已有 ONNX：

    python convert.py export --model-version dmc61sbr_reglu --model-type onnx \\
        --model-width 640 --model-height 368 --target-device generic

本工具再：

1. 将 ``q_index_shifted`` 折成 ``--qp`` 常量（对齐 ``node_mlvc.c`` 不喂 qp）
2. 可选把 SpaceToDepth / Max / Div 换成 NPU 友好算子
3. 默认把解码器尾部 DepthToSpace+Clip 拆出图外（CPU DCR，板上 1:1 且更快）
4. 用 rknn-toolkit2 转 FP16 ``.rknn``
5. 把 ``gaussian_pmf.json`` / ``bit_estimator_pmf.json`` 写成 ``gaussian.bin`` / ``bitest.bin``

详见 ``docs/mlvc-rknn-export.md``。
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence

_DIR = Path(__file__).resolve().parent
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))

import pmf  # noqa: E402
import qppatch  # noqa: E402
import rknn_convert  # noqa: E402
import export_onnx  # noqa: E402
from onnx_rewrite import (  # noqa: E402
    OnnxRewriteError,
    classify_part,
    inspect_onnx,
    prepare_onnx,
    validate_runtime_io,
)
from onnx_extract import extract_trailing_depth_to_space  # noqa: E402


class ExportError(RuntimeError):
    pass


ENC_ONNX_NAMES = ("MLVCEncoder.onnx", "MLVCEncoder_rk3588.onnx")
DEC_ONNX_NAMES = ("MLVCDecoder.onnx", "MLVCDecoder_rk3588.onnx")
GAUSS_JSON_NAMES = ("gaussian_pmf.json", "gaussian.json")
BITEST_JSON_NAMES = ("bit_estimator_pmf.json", "bitest.json", "bit_estimator.json")


def _first_file(directory: Path, names: Sequence[str]) -> Path | None:
    for name in names:
        cand = directory / name
        if cand.is_file():
            return cand
    for name in names:
        hits = sorted(p for p in directory.rglob(name) if p.is_file())
        if hits:
            return hits[0]
    return None


def _first_glob(directory: Path, pattern: str, *, exclude: Iterable[str] = ()) -> Path | None:
    hits = sorted(
        p
        for p in directory.glob(pattern)
        if p.is_file() and p.name not in set(exclude)
    )
    return hits[0] if hits else None


def find_sources(onnx_dir: Path) -> dict[str, Path | None]:
    if not onnx_dir.is_dir():
        raise ExportError(f"ONNX 目录不存在: {onnx_dir}")
    encoder = _first_file(onnx_dir, ENC_ONNX_NAMES) or _first_glob(
        onnx_dir, "*Encoder*.onnx"
    )
    decoder = _first_file(onnx_dir, DEC_ONNX_NAMES) or _first_glob(
        onnx_dir, "*Decoder*.onnx"
    )
    gaussian = _first_file(onnx_dir, GAUSS_JSON_NAMES)
    bitest = _first_file(onnx_dir, BITEST_JSON_NAMES)
    return {
        "encoder": encoder,
        "decoder": decoder,
        "gaussian": gaussian,
        "bitest": bitest,
    }


def _parse_qp_list(text: str | None, default_qp: int) -> list[int]:
    if not text:
        return [default_qp]
    out: list[int] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        qp = int(part)
        if qp < 0:
            raise ExportError(f"qp 不能为负: {qp}")
        if qp not in out:
            out.append(qp)
    if not out:
        raise ExportError("--qp-list 为空")
    return out


def default_output_dir(model_version: str) -> Path:
    """让标准版与轻量版落到平行、自包含的模型 bundle。"""
    variant = (
        "mlvc-s"
        if model_version == export_onnx.DEFAULT_S_MODEL_VERSION
        else "mlvc"
    )
    return Path("models") / variant


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _rknn_name(part: str, platform: str) -> str:
    stem = "MLVCEncoder" if part == "encoder" else "MLVCDecoder"
    return f"{stem}_{platform}.rknn"


def export_pmf(sources: dict[str, Path | None], out_dir: Path) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if sources.get("gaussian"):
        meta = pmf.convert_json_file(sources["gaussian"], out_dir / "gaussian.bin", kind="gaussian")
        result["gaussian"] = meta
        print(f"  gaussian.bin  {meta['bytes']} B  (levels={meta['scale_levels']}, index_space={meta['index_space']})")
        if not meta.get("index_space"):
            print("  警告: node_mlvc.c 要求 gaussian.bin 的 index_space=1，当前为 0")
    if sources.get("bitest"):
        meta = pmf.convert_json_file(sources["bitest"], out_dir / "bitest.bin", kind="bitest")
        result["bitest"] = meta
        print(f"  bitest.bin    {meta['bytes']} B  (qp_num={meta['qp_num']}, channels={meta['channels']})")
    if not result:
        print("  （未找到 gaussian_pmf.json / bit_estimator_pmf.json，跳过 PMF）")
    return result


def export_models(
    *,
    sources: dict[str, Path | None],
    out_dir: Path,
    platform: str,
    qp_list: Sequence[int],
    rewrite: bool,
    fold: bool,
    keep_onnx: bool,
    verbose: bool,
    skip_rknn: bool,
    extract_tail: bool,
    default_qp: int | None = None,
) -> dict[str, Any]:
    keep_prepared = keep_onnx or skip_rknn
    work = Path(tempfile.mkdtemp(prefix="rkvc_mlvc_onnx_"))
    models_meta: dict[str, Any] = {}
    try:
        for part in ("encoder", "decoder"):
            src = sources.get(part)
            if src is None:
                print(f"  （未找到 {part} ONNX，跳过）")
                continue
            print(f"== {part}: {src.name} ==")
            part_meta: dict[str, Any] = {"source": str(src), "qps": {}}
            for qp in qp_list:
                prepared = work / f"{part}_qp{qp}.onnx"
                try:
                    info, report = prepare_onnx(
                        src, prepared, qp=qp if fold else None, rewrite=rewrite, fold=fold
                    )
                except OnnxRewriteError as exc:
                    raise ExportError(str(exc)) from exc
                extract_note = ""
                if extract_tail and part == "decoder":
                    onnx_mod = __import__("onnx")
                    model = onnx_mod.load(str(prepared))
                    try:
                        ex = extract_trailing_depth_to_space(model)
                    except OnnxRewriteError as exc:
                        raise ExportError(str(exc)) from exc
                    onnx_mod.save(model, str(prepared))
                    info = inspect_onnx(prepared)
                    extract_note = (
                        f"  tail-D2S bs={ex.blocksize} mode={ex.mode or 'DCR'} "
                        f"{ex.in_shape}"
                    )
                kind = classify_part(info)
                problems = validate_runtime_io(info, part=kind if kind != "unknown" else part)
                print(
                    f"  qp={qp}: fold={report.folded_inputs or '-'}  "
                    f"SpaceToDepth={report.space_to_depth} Max→Clip={report.max_to_clip} "
                    f"Div→Mul={report.div_to_mul}"
                    f"{extract_note}"
                )
                if report.skipped:
                    for item in report.skipped[:8]:
                        print(f"    skip: {item}")
                    if len(report.skipped) > 8:
                        print(f"    skip: …另 {len(report.skipped) - 8} 条")
                for prob in problems:
                    print(f"    警告: {prob}")
                if problems:
                    raise ExportError(
                        "ONNX I/O 与运行时约定不符: " + "; ".join(problems)
                    )

                rknn_rel = _rknn_name(part, platform)
                if len(qp_list) == 1:
                    rknn_path = out_dir / rknn_rel
                else:
                    rknn_path = out_dir / f"{platform}_qp_models" / f"qp{qp}" / rknn_rel

                qp_entry: dict[str, Any] = {
                    "rknn_precision": "fp16",
                    "quantized": False,
                    "inputs": [{"name": t.name, "shape": t.shape, "dtype": t.dtype} for t in info.inputs],
                    "outputs": [{"name": t.name, "shape": t.shape, "dtype": t.dtype} for t in info.outputs],
                    "rewrite": {
                        "folded_inputs": report.folded_inputs,
                        "space_to_depth": report.space_to_depth,
                        "max_to_clip": report.max_to_clip,
                        "min_to_clip": report.min_to_clip,
                        "div_to_mul": report.div_to_mul,
                        "extract_tail": bool(extract_note),
                        "skipped": report.skipped,
                    },
                    "io_warnings": problems,
                }
                if skip_rknn:
                    onnx_out = rknn_path.with_suffix(".onnx")
                    onnx_out.parent.mkdir(parents=True, exist_ok=True)
                    onnx_out.write_bytes(prepared.read_bytes())
                    qp_entry["prepared_onnx"] = str(onnx_out)
                    print(f"    → {onnx_out}（--skip-rknn）")
                else:
                    input_names = [t.name for t in info.inputs]
                    input_sizes: list[list[int]] = []
                    for t in info.inputs:
                        if any(d is None for d in t.shape):
                            input_sizes = []
                            break
                        input_sizes.append([int(d) for d in t.shape])  # type: ignore[arg-type]
                    try:
                        produced = rknn_convert.convert_onnx_to_rknn(
                            prepared,
                            rknn_path,
                            target=platform,
                            verbose=verbose,
                            input_names=input_names or None,
                            input_size_list=input_sizes or None,
                        )
                    except rknn_convert.RknnConvertError as exc:
                        raise ExportError(str(exc)) from exc
                    qp_entry["rknn"] = str(produced)
                    qp_entry["rknn_bytes"] = produced.stat().st_size
                    print(f"    → {produced}  ({produced.stat().st_size} B)")
                    if keep_prepared:
                        kept = rknn_path.with_suffix(".onnx")
                        kept.write_bytes(prepared.read_bytes())
                        qp_entry["kept_onnx"] = str(kept)
                part_meta["qps"][str(qp)] = qp_entry
            models_meta[part] = part_meta

            if not skip_rknn and len(qp_list) > 1:
                primary = default_qp if default_qp in qp_list else qp_list[0]
                src_rknn = Path(part_meta["qps"][str(primary)]["rknn"])
                dst_rknn = out_dir / _rknn_name(part, platform)
                dst_rknn.write_bytes(src_rknn.read_bytes())
                print(f"  默认拷贝 qp={primary} → {dst_rknn}")
                part_meta["default_rknn"] = str(dst_rknn)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return models_meta


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="将 MLVC ONNX/PMF 导出为 rkvc 使用的 RKNN 模型与 PMF1 表",
    )
    p.add_argument("--onnx-dir", type=Path, help="microsoft/mlvc convert.py 输出目录（含 MLVCEncoder.onnx 等）")
    p.add_argument("--encoder", type=Path, help="编码器 ONNX 路径")
    p.add_argument("--decoder", type=Path, help="解码器 ONNX 路径")
    p.add_argument("--gaussian-json", type=Path, help="gaussian_pmf.json")
    p.add_argument("--bitest-json", type=Path, help="bit_estimator_pmf.json")
    p.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="输出 bundle 目录（默认按模型版本选择 models/mlvc 或 models/mlvc-s）",
    )
    p.add_argument("--platform", default="rk3588", help="RKNN target_platform（默认 rk3588）")
    p.add_argument("--qp", type=int, default=21, help="折叠进图的 q_index（默认 21，与 node_mlvc 默认 qp 一致）")
    p.add_argument(
        "--qp-list",
        default=None,
        help="逗号分隔多个 qp，写入 {platform}_qp_models/qpXX/（现网目录名为 rk3588_qp_models）",
    )
    p.add_argument("--no-rewrite", action="store_true", help="不做 SpaceToDepth/Max/Div 图重写")
    p.add_argument(
        "--no-extract-tail",
        action="store_true",
        help="保留解码器尾部 DepthToSpace+Clip 在图内（默认拆到 CPU，板上 1:1 且更快）",
    )
    p.add_argument("--no-fold-qp", action="store_true", help="保留 q_index 输入（C 运行时目前不能喂该输入）")
    p.add_argument("--keep-onnx", action="store_true", help="在输出目录保留折叠/重写后的 ONNX")
    p.add_argument("--skip-rknn", action="store_true", help="只做 PMF + ONNX 图处理，不调用 rknn-toolkit2")
    p.add_argument("--make-patches", action="store_true",
                   help="多 qp 导出后生成 QPP1 补丁到 --patch-dir（需已产出 .rknn）")
    p.add_argument("--patch-dir", type=Path, default=None,
                   help="QPP1 输出目录（默认 {out-dir}/qp_patches）")
    p.add_argument("--pmf-only", action="store_true", help="只转换 PMF JSON")
    p.add_argument("--inspect", action="store_true", help="只打印 ONNX I/O，不转换")
    p.add_argument("--verbose", action="store_true", help="RKNN 转换详细日志")
    p.add_argument("--from-mlvc", action="store_true",
                   help="先跑 microsoft/mlvc convert.py 导出 ONNX（浅克隆到 .build/deps/mlvc）")
    p.add_argument("--mlvc-dir", type=Path, default=None, help="microsoft/mlvc 源码目录")
    p.add_argument("--mlvc-data-dir", type=Path, default=None, help="权重与占位 YUV（默认 .build/deps/mlvc-data）")
    p.add_argument(
        "--weights-path", type=Path, default=None,
        help="覆盖 MLVC checkpoint（MLVC-S 必填）",
    )
    p.add_argument("--model-version", default=export_onnx.DEFAULT_MODEL_VERSION)
    p.add_argument("--weights-version", default=None, help="覆盖 checkpoint 版本名（默认随 --model-version）")
    p.add_argument("--model-width", type=int, default=export_onnx.DEFAULT_WIDTH)
    p.add_argument("--model-height", type=int, default=export_onnx.DEFAULT_HEIGHT)
    p.add_argument("--target-device", default="generic", help="convert.py --target-device（必须 generic 或 intel）")
    p.add_argument("--onnx-frame-count", type=int, default=2, help="convert.py tracing 帧数")
    return p


def resolve_sources(args: argparse.Namespace) -> dict[str, Path | None]:
    sources: dict[str, Path | None] = {
        "encoder": None,
        "decoder": None,
        "gaussian": None,
        "bitest": None,
    }
    if args.onnx_dir:
        sources.update(find_sources(args.onnx_dir))
    if args.encoder:
        sources["encoder"] = args.encoder
    if args.decoder:
        sources["decoder"] = args.decoder
    if args.gaussian_json:
        sources["gaussian"] = args.gaussian_json
    if args.bitest_json:
        sources["bitest"] = args.bitest_json
    return sources


def cmd_inspect(sources: dict[str, Path | None]) -> int:
    any_onnx = False
    for part in ("encoder", "decoder"):
        src = sources.get(part)
        if src is None:
            continue
        any_onnx = True
        info = inspect_onnx(src)
        kind = classify_part(info)
        print(f"== {part} ({kind})  {src} ==")
        print(f"  opset {info.opset}")
        for spec in info.inputs:
            print(f"  IN  {spec.name:24s} {spec.dtype:8s} {spec.shape}")
        for spec in info.outputs:
            print(f"  OUT {spec.name:24s} {spec.dtype:8s} {spec.shape}")
        for warn in validate_runtime_io(info, part=kind if kind != "unknown" else part):
            print(f"  警告: {warn}")
    if not any_onnx:
        raise ExportError("没有可 inspect 的 ONNX")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.from_mlvc:
        onnx_dir = export_onnx.export_onnx(
            mlvc_dir=args.mlvc_dir,
            data_dir=args.mlvc_data_dir,
            weights_path=args.weights_path,
            model_version=args.model_version,
            weights_version=args.weights_version,
            width=args.model_width,
            height=args.model_height,
            target_device=args.target_device,
            frame_count=args.onnx_frame_count,
        )
        if args.onnx_dir is None:
            args.onnx_dir = onnx_dir
        print(f"ONNX 目录: {onnx_dir}")
    if not args.onnx_dir and not any(
        [args.encoder, args.decoder, args.gaussian_json, args.bitest_json]
    ):
        build_parser().print_help()
        return 2

    sources = resolve_sources(args)
    if args.inspect:
        return cmd_inspect(sources)

    out_dir = args.out_dir or default_output_dir(args.model_version)
    out_dir.mkdir(parents=True, exist_ok=True)
    qp_list = _parse_qp_list(args.qp_list, args.qp)

    print(f"输出目录: {out_dir.resolve()}")
    print("PMF:")
    pmf_meta = export_pmf(sources, out_dir)
    if args.pmf_only:
        return 0

    if not args.skip_rknn and (sources.get("encoder") or sources.get("decoder")):
        rknn_convert.require_rknn()

    models_meta = export_models(
        sources=sources,
        out_dir=out_dir,
        platform=args.platform,
        qp_list=qp_list,
        rewrite=not args.no_rewrite,
        fold=not args.no_fold_qp,
        keep_onnx=args.keep_onnx,
        verbose=args.verbose,
        skip_rknn=args.skip_rknn,
        extract_tail=not args.no_extract_tail,
        default_qp=args.qp,
    )
    patch_meta = None
    qp_models = out_dir / f"{args.platform}_qp_models"
    want_patches = args.make_patches or (len(qp_list) > 1 and not args.skip_rknn)
    if want_patches:
        patch_dir = args.patch_dir or (out_dir / "qp_patches")
        if args.skip_rknn:
            print("  --make-patches 需要 .rknn，已跳过")
        elif not qp_models.is_dir():
            msg = f"未找到 {qp_models}（请用 --qp-list 导出多个 qp）"
            if args.make_patches:
                raise ExportError("--make-patches：" + msg)
            print("  --make-patches：" + msg)
        else:
            print("QPP1 补丁:")
            patch_meta = qppatch.generate_from_qp_dir(
                qp_models, patch_dir,
                base_qp=args.qp if args.qp in qp_list else qp_list[0],
            )
    manifest = {
        "generated_at_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "platform": args.platform,
        "qp_list": qp_list,
        "rewrite": not args.no_rewrite,
        "fold_qp": not args.no_fold_qp,
        "rknn_export": {
            "precision": "fp16",
            "do_quantization": False,
            "skipped": args.skip_rknn,
        },
        "pmf": pmf_meta,
        "models": models_meta,
        "qppatch": patch_meta,
        "extract_tail": not args.no_extract_tail,
        "runtime": "lib/node_mlvc.c（编码器用混合 I/O：2 输入走 native set_io_mem、输出为逻辑 NCHW；解码器 4 输入与输出走 native NC1HWC2；x_hat 若为 3·bs² 通道则 CPU DepthToSpace DCR）",
    }
    _write_json(out_dir / "mlvc_rknn_export_manifest.json", manifest)
    print(f"清单: {out_dir / 'mlvc_rknn_export_manifest.json'}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ExportError, pmf.PmfError, OnnxRewriteError, rknn_convert.RknnConvertError, qppatch.QppatchError, export_onnx.OnnxExportError) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
