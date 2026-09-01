#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""浅克隆 microsoft/mlvc 并调用 ``convert.py export``，产出 RKNN 转换用的 ONNX + PMF JSON。

不把上游训练代码 vendoring 进本仓。检出与权重落在 ``.build/deps/``。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path
from typing import Sequence

MLVC_GIT = "https://github.com/microsoft/mlvc.git"
CKPT_URL = "https://mlvideopub.blob.core.windows.net/mlvc/models/mlvc-psnr-v1.ckpt"
CKPT_SHA256 = "834adaab680837c4106a9bb62e5778b3f13729b4fe6f751797e0747214f5cca1"
# MLVC-S 权重与标准版同在 mlvideopub 公开容器（匿名可读）；属上游非承诺资源，
# 失效时可手动放置该文件或改用 --weights-path。
CKPT_S_URL = "https://mlvideopub.blob.core.windows.net/mlvc/models/mlvc-s-psnr-v1.ckpt"
CKPT_S_SHA256 = "1b86b757ddb115342293efb57719d6216c6ee2e459ae796ec41723b5c05ca896"
DEFAULT_MODEL_VERSION = "dmc61sbr_reglu"
DEFAULT_S_MODEL_VERSION = "dmc61sbr_reglu_s"
DEFAULT_WEIGHTS_VERSION = "mlvc-psnr-v1"
DEFAULT_S_WEIGHTS_VERSION = "mlvc-s-psnr-v1"
DEFAULT_WIDTH = 640
DEFAULT_HEIGHT = 368
# convert.py 对 640×368 选用的默认测试序列（见 video/conversion/const.py）
DEFAULT_TEST_YUV_REL = Path(
    "yuv/640x360_30fps/s1/0380a333cb0fef69001fb4260e2a705f_640x360_30fps.yuv"
)
class OnnxExportError(RuntimeError):
    pass


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_mlvc_dir() -> Path:
    return repo_root() / ".build" / "deps" / "mlvc"


def default_data_dir() -> Path:
    return repo_root() / ".build" / "deps" / "mlvc-data"


def uv_bin() -> Path:
    """定位 uv：RKVC_UV → PATH → .build/host/uv-bootstrap。"""
    if os.environ.get("RKVC_UV"):
        return Path(os.environ["RKVC_UV"])
    found = shutil.which("uv")
    if found:
        return Path(found)
    bootstrap = repo_root() / ".build" / "host" / "uv-bootstrap" / "bin" / "uv"
    if bootstrap.is_file():
        return bootstrap
    raise OnnxExportError(
        "找不到 uv（PATH 无 uv 且 .build/host/uv-bootstrap/bin/uv 不存在）"
    )


def _run(cmd: Sequence[str], *, cwd: Path | None = None) -> None:
    print("+", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=cwd)
    if proc.returncode != 0:
        raise OnnxExportError(f"命令失败 ({proc.returncode}): {' '.join(cmd)}")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download_file(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    print(f"下载 {url} → {dest}")
    curl = shutil.which("curl")
    if curl:
        _run([curl, "-L", "--fail", "--retry", "3", "-o", str(tmp), url])
    else:
        urllib.request.urlretrieve(url, tmp)
    tmp.replace(dest)


def write_dummy_yuv420(path: Path, width: int, height: int, frames: int = 2) -> None:
    """I420：Y=16、UV=128 的灰帧，供 convert.py 采集 tracing 样本。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    y = bytes([16]) * (width * height)
    u = bytes([128]) * (width * height // 4)
    v = bytes([128]) * (width * height // 4)
    frame = y + u + v
    with path.open("wb") as fp:
        for _ in range(frames):
            fp.write(frame)


def _uv_environment_marker() -> str:
    return f"sys_platform == '{sys.platform}' and platform_machine == '{platform.machine()}'"


def _force_cpu_torch_source(text: str, package: str) -> str:
    """Bind an upstream PyTorch package to the CPU-only index."""
    pattern = rf"(?ms)^{re.escape(package)}\s*=\s*\[.*?^\]\s*"
    replacement = f'{package} = [\n    {{ index = "pytorch-cpu" }},\n]\n'
    new, count = re.subn(pattern, replacement, text, count=1)
    if count == 0:
        raise OnnxExportError(
            f"无法在 microsoft/mlvc [tool.uv.sources] 中找到 {package} source"
        )
    return new


def _remove_cuda_torch_indexes(text: str) -> str:
    """Remove explicit CUDA indexes so later dependency changes cannot select them."""
    pattern = (
        r'(?ms)^\[\[tool\.uv\.index\]\]\s*\n'
        r'name\s*=\s*"pytorch-cu[^"]*"\s*\n'
        r'.*?(?=^\[\[tool\.uv\.index\]\]|^\[tool\.|\Z)'
    )
    return re.sub(pattern, "", text)


def patch_mlvc_pyproject(text: str) -> str:
    """只解析本机 environment，并强制使用 CPU-only PyTorch wheel。"""
    env = _uv_environment_marker()
    desired_envs = f'environments = [\n    "{env}",\n]'
    m = re.search(r"environments = \[[^\]]*\]", text, re.S)
    if not m:
        raise OnnxExportError("无法在 microsoft/mlvc pyproject.toml 中找到 [tool.uv] environments")
    if m.group(0) != desired_envs:
        text = text[: m.start()] + desired_envs + text[m.end() :]

    if '"coremltools"' in text and "coremltools;" not in text:
        text = text.replace(
            '"coremltools"',
            '"coremltools; sys_platform == \'darwin\'"',
            1,
        )

    gpu = (
        '"onnxruntime-gpu; sys_platform == \'linux\' or '
        "(sys_platform == 'win32' and platform_machine == 'AMD64')\""
    )
    gpu_fixed = (
        '"onnxruntime-gpu; (sys_platform == \'linux\' and platform_machine == \'x86_64\') or '
        "(sys_platform == 'win32' and platform_machine == 'AMD64')\""
    )
    if gpu in text:
        text = text.replace(gpu, gpu_fixed, 1)

    if "[tool.uv.sources]" not in text or 'name = "pytorch-cpu"' not in text:
        raise OnnxExportError("microsoft/mlvc 缺少 pytorch-cpu uv index")
    text = _force_cpu_torch_source(text, "torch")
    if re.search(r"(?m)^torchvision\s*=\s*\[", text):
        text = _force_cpu_torch_source(text, "torchvision")
    text = _remove_cuda_torch_indexes(text)
    return text


def patch_download_helpers(src: str) -> str:
    """相对路径若在 --test-data-dir / --job-outputs-dir 下已存在，则不访问 Azure。"""
    if "if local.is_file():" in src:
        return src
    pattern = (
        r"(def download_(?:test_data|job_outputs)\(path: Path \| str, "
        r"base_path: Path \| str = DEFAULT_\w+\) -> Path:\n)"
        r"([ \t]*)if Path\(path\)\.is_absolute\(\):\n"
        r"([ \t]*)return Path\(path\)\n"
    )

    def repl(match: re.Match) -> str:
        ind = match.group(2)
        inner = match.group(3)
        return (
            match.group(1)
            + f"{ind}local = Path(base_path).expanduser() / path\n"
            + f"{ind}if Path(path).is_absolute() and Path(path).is_file():\n"
            + f"{inner}return Path(path)\n"
            + f"{ind}if local.is_file():\n"
            + f"{inner}return local\n"
            + f"{ind}if Path(path).is_absolute():\n"
            + f"{inner}return Path(path)\n"
        )

    new, n = re.subn(pattern, repl, src, count=2)
    if n < 2:
        raise OnnxExportError(f"无法给 conversion/utils.py 打本地路径补丁（匹配 {n} 次）")
    return new


MLVC_S_MODEL_CONFIG = """
dmc61sbr_reglu_s:
  module: ._dmc61sb_model
  class: TraceableMLVC
  weights_path: pretrained/mlvc-s-psnr-v1.ckpt
  weights_version: mlvc-s-psnr-v1
  iframe_period: 64
  reset_period: null
  split_type: dmc61sbr_e1d1
  params:
    disable_feature_reset: true
    depth_conv_block_params:
      activation: LeakyReLU
      zero_init_residual: true
      chunk_mode: gated
      ffn_gate_activation: ReLU1
    feature_channels: 48
    spatial_prior_channels: 128
    input_offset: -0.5
    memory_activation: identity
    chain_feature_adaptors: true
    recon_channels: 192
    hidden_channels: 192
    hyperprior_num_blocks: 2
    y_scale_repeat: 4
    z_channels: 48
    y_channels: 48
    hyperprior_variant: mini
    feature_extractor_num_conv1_layers: 1
    feature_extractor_num_conv2_layers: 1
"""


def ensure_mlvc_s_model_config(mlvc_dir: Path) -> None:
    """把 MLVC-S（DMC-6.1SB 窄通道）写进 conversion model_configs.yaml。"""
    dest = mlvc_dir / "video" / "conversion" / "_full_model" / "model_configs.yaml"
    if not dest.is_file():
        return
    text = dest.read_text(encoding="utf-8")
    if re.search(r"^dmc61sbr_reglu_s\s*:", text, re.M):
        return
    dest.write_text(text.rstrip() + "\n" + MLVC_S_MODEL_CONFIG, encoding="utf-8")
    print(f"已写入 MLVC-S 转换配置: {dest}")


def default_weights_version(model_version: str) -> str:
    if model_version == DEFAULT_S_MODEL_VERSION:
        return DEFAULT_S_WEIGHTS_VERSION
    return DEFAULT_WEIGHTS_VERSION


def patch_mlvc_checkout(mlvc_dir: Path) -> None:
    """让当前平台能 uv sync，并优先用本地权重/测试 YUV（不强制 Azure）。"""
    pyproject = mlvc_dir / "pyproject.toml"
    orig = pyproject.read_text(encoding="utf-8")
    text = patch_mlvc_pyproject(orig)
    if text != orig:
        pyproject.write_text(text, encoding="utf-8")
        print("已补丁 pyproject.toml（当前平台 environment / CPU-only PyTorch / coremltools）")

    example = mlvc_dir / "video" / "conversion" / "_full_model" / "model_configs_example.yaml"
    dest = mlvc_dir / "video" / "conversion" / "_full_model" / "model_configs.yaml"
    if example.is_file() and not dest.exists():
        shutil.copy2(example, dest)
        print(f"已复制 {dest.name}")

    utils = mlvc_dir / "video" / "conversion" / "utils.py"
    src = utils.read_text(encoding="utf-8")
    patched = patch_download_helpers(src)
    if patched != src:
        utils.write_text(patched, encoding="utf-8")
        print("已补丁 conversion/utils.py：本地文件优先于 Azure")

    bundler = mlvc_dir / "video" / "conversion" / "_model_bundler.py"
    if bundler.is_file():
        bsrc = bundler.read_text(encoding="utf-8")
        if "import coremltools as ct" in bsrc and "ONNX 导出不需要 CoreML" not in bsrc:
            bnew = bsrc.replace(
                "import coremltools as ct\n",
                "try:\n    import coremltools as ct\nexcept ImportError:\n    ct = None  # ONNX 导出不需要 CoreML\n",
                1,
            )
            if bnew == bsrc:
                raise OnnxExportError(f"无法给 {bundler} 打 coremltools 可选导入补丁")
            bundler.write_text(bnew, encoding="utf-8")
            print("已补丁 conversion/_model_bundler.py：coremltools 可选")

    exporter = mlvc_dir / "video" / "conversion" / "_exporter" / "_onnx_exporter.py"
    if exporter.is_file():
        esrc = exporter.read_text(encoding="utf-8")
        # torch.onnx.export(dynamo=...) 需要 torch>=2.4；本板 CPU 只能用 2.2（2.10 SIGILL）
        if "dynamo=False," in esrc:
            exporter.write_text(
                esrc.replace("            dynamo=False,\n", ""),
                encoding="utf-8",
            )
            print("已补丁 _onnx_exporter.py：去掉 torch.onnx.export(dynamo=)（兼容 torch 2.2）")

    # 本地 convert.py 不装 Azure SDK：推迟 import azure（幂等）
    factory = mlvc_dir / "video" / "conversion" / "_full_model" / "_model_factory.py"
    if factory.is_file():
        fsrc = factory.read_text(encoding="utf-8")
        factory_top = (
            "from ..const import DEFAULT_JOB_OUTPUTS_DIR\nfrom .._azure import get_azureml_job\n"
        )
        if factory_top in fsrc:
            fsrc = fsrc.replace(factory_top, "from ..const import DEFAULT_JOB_OUTPUTS_DIR\n", 1)
            fsrc = fsrc.replace(
                "                job = get_azureml_job(job_name)\n",
                "                from .._azure import get_azureml_job\n\n"
                "                job = get_azureml_job(job_name)\n",
                1,
            )
            factory.write_text(fsrc, encoding="utf-8")
            print("已补丁 _model_factory.py：Azure 延迟导入")

    utils = mlvc_dir / "video" / "conversion" / "utils.py"
    if utils.is_file():
        usrc = utils.read_text(encoding="utf-8")
        utils_top = "from typing import NamedTuple, Any\nfrom ._azure import download_blob\n"
        if utils_top in usrc:
            usrc = usrc.replace(
                utils_top,
                "from typing import NamedTuple, Any\n",
                1,
            )
            usrc = usrc.replace(
                "    return download_blob(\n",
                "    from ._azure import download_blob\n\n    return download_blob(\n",
            )
            utils.write_text(usrc, encoding="utf-8")
            print("已补丁 conversion/utils.py：Azure 延迟导入")

    tester = mlvc_dir / "video" / "conversion" / "_model_tester.py"
    if tester.is_file():
        tsrc = tester.read_text(encoding="utf-8")
        tester_top = (
            "from dataclasses import dataclass\n"
            "from azure.core.exceptions import ResourceNotFoundError\n"
        )
        if tester_top in tsrc:
            tsrc = tsrc.replace(tester_top, "from dataclasses import dataclass\n", 1)
            marker = (
                "    def _load_azureml_results(self, conversion_metadata: ConversionMetadata)"
                " -> list[FrameLoopSummary]:\n"
            )
            inject = (
                marker
                + "        try:\n"
                + "            from azure.core.exceptions import ResourceNotFoundError\n"
                + "        except ImportError:\n"
                + "            ResourceNotFoundError = Exception  # 本地导出不装 Azure SDK\n"
            )
            if marker in tsrc and "ResourceNotFoundError = Exception" not in tsrc:
                tsrc = tsrc.replace(marker, inject, 1)
            tester.write_text(tsrc, encoding="utf-8")
            print("已补丁 _model_tester.py：Azure 延迟导入")

    ensure_mlvc_s_model_config(mlvc_dir)


def ensure_mlvc_src(mlvc_dir: Path) -> Path:
    convert_py = mlvc_dir / "video" / "convert.py"
    if convert_py.is_file():
        print(f"使用已有 MLVC 源码: {mlvc_dir}")
    else:
        mlvc_dir.parent.mkdir(parents=True, exist_ok=True)
        if mlvc_dir.exists():
            shutil.rmtree(mlvc_dir)
        _run(["git", "clone", "--depth", "1", MLVC_GIT, str(mlvc_dir)])
    patch_mlvc_checkout(mlvc_dir)
    return mlvc_dir


def ensure_checkpoint(
    data_dir: Path,
    weights_path: Path | None,
    model_version: str = DEFAULT_MODEL_VERSION,
) -> Path:
    if weights_path is not None:
        if not weights_path.is_file():
            raise OnnxExportError(f"权重不存在: {weights_path}")
        return weights_path
    if model_version == DEFAULT_S_MODEL_VERSION:
        dest = data_dir / "pretrained" / "mlvc-s-psnr-v1.ckpt"
        if dest.is_file() and sha256_file(dest) == CKPT_S_SHA256:
            print(f"权重已就绪: {dest}")
            return dest
        try:
            download_file(CKPT_S_URL, dest)
        except Exception as exc:
            raise OnnxExportError(
                f"MLVC-S 权重下载失败: {exc}\n"
                f"该地址（{CKPT_S_URL}）为上游非承诺公开资源；\n"
                f"可手动放置 {dest} 或用 --weights-path 指定 mlvc-s-psnr-v1.ckpt"
            ) from exc
        digest = sha256_file(dest)
        if digest != CKPT_S_SHA256:
            dest.unlink(missing_ok=True)
            raise OnnxExportError(f"MLVC-S 权重 SHA-256 不符: {digest}")
        return dest
    dest = data_dir / "pretrained" / "mlvc-psnr-v1.ckpt"
    if dest.is_file() and sha256_file(dest) == CKPT_SHA256:
        print(f"权重已就绪: {dest}")
        return dest
    download_file(CKPT_URL, dest)
    digest = sha256_file(dest)
    if digest != CKPT_SHA256:
        dest.unlink(missing_ok=True)
        raise OnnxExportError(f"权重 SHA-256 不符: {digest}")
    return dest


def ensure_test_yuv(test_data_dir: Path, width: int = 640, height: int = 360) -> Path:
    path = test_data_dir / DEFAULT_TEST_YUV_REL
    expect = width * height * 3 // 2
    if path.is_file() and path.stat().st_size >= expect:
        print(f"测试 YUV 已就绪: {path}")
        return path
    write_dummy_yuv420(path, width, height, frames=2)
    print(f"写入占位 YUV（tracing 用）: {path}")
    return path


def project_venv_python() -> Path:
    return repo_root() / ".venv" / "bin" / "python"


def _venv_ok() -> bool:
    py = project_venv_python()
    if not py.is_file():
        return False
    proc = subprocess.run(
        [
            str(py),
            "-c",
            "import torch, onnx; assert torch.version.cuda is None",
        ],
        capture_output=True,
        text=True,
    )
    return proc.returncode == 0


def ensure_project_venv(mlvc_dir: Path) -> Path:
    """使用仓库根目录唯一的 ``.venv``；msrtc.rans 从 microsoft/mlvc 源码装进该环境。"""
    py = project_venv_python()
    if not py.is_file():
        raise OnnxExportError(
            f"仓库根目录没有 .venv（{py}）。请先运行: uv sync"
        )
    if not _venv_ok():
        raise OnnxExportError(
            f"{py} 缺少 CPU-only torch/onnx。请在仓库根目录运行: uv sync"
        )
    rans = mlvc_dir / "packages" / "msrtc_rans"
    if not rans.is_dir():
        raise OnnxExportError(f"缺少 {rans}（microsoft/mlvc 熵编码器）")
    chk = subprocess.run(
        [str(py), "-c", "import msrtc.rans"],
        capture_output=True,
        text=True,
    )
    if chk.returncode != 0:
        _run(
            [
                str(uv_bin()),
                "pip",
                "install",
                "--python",
                str(py),
                str(rans),
            ]
        )
    chk = subprocess.run(
        [str(py), "-c", "import msrtc.rans"],
        capture_output=True,
        text=True,
    )
    if chk.returncode != 0:
        raise OnnxExportError(
            f"无法把 msrtc.rans 装进 {py}（{chk.stderr.strip() or chk.stdout.strip()}）"
        )
    print(f"使用项目 venv: {py}")
    return py


def find_exported_onnx(export_root: Path) -> Path:
    hits = sorted(p for p in export_root.rglob("MLVCEncoder.onnx") if p.is_file())
    if not hits:
        raise OnnxExportError(f"未找到 MLVCEncoder.onnx（在 {export_root}）")
    return hits[0].parent


def export_onnx(
    *,
    mlvc_dir: Path | None = None,
    data_dir: Path | None = None,
    weights_path: Path | None = None,
    model_version: str = DEFAULT_MODEL_VERSION,
    weights_version: str | None = None,
    width: int = DEFAULT_WIDTH,
    height: int = DEFAULT_HEIGHT,
    target_device: str = "generic",
    frame_count: int = 2,
    output_path: Path | None = None,
    skip_if_exists: bool = True,
) -> Path:
    if target_device == "qualcomm":
        raise OnnxExportError("不要用 --target-device qualcomm（RKNN 上会 CPU fallback）")
    mlvc_dir = ensure_mlvc_src(mlvc_dir or default_mlvc_dir()).resolve()
    data_dir = (data_dir or default_data_dir()).resolve()
    data_dir.mkdir(parents=True, exist_ok=True)
    test_data_dir = data_dir / "test-set"
    ckpt = ensure_checkpoint(data_dir, weights_path, model_version)
    ensure_test_yuv(test_data_dir)
    py = ensure_project_venv(mlvc_dir)

    out = (output_path or (mlvc_dir / "video" / "output" / "models")).resolve()
    if skip_if_exists:
        try:
            existing = find_exported_onnx(out)
            if (existing / "MLVCDecoder.onnx").is_file() and (existing / "gaussian_pmf.json").is_file():
                print(f"已有 ONNX，跳过 convert.py: {existing}")
                return existing
        except OnnxExportError:
            pass

    cmd = [
        str(py),
        "convert.py",
        "--job-outputs-dir", str(data_dir),
        "--test-data-dir", str(test_data_dir),
        "export",
        "--model-version", model_version,
        "--model-type", "onnx",
        "--target-device", target_device,
        "--model-width", str(width),
        "--model-height", str(height),
        "--frame-count", str(frame_count),
        "--output-path", str(out),
        "--weights-path", str(ckpt.resolve()),
        "--weights-version", weights_version or default_weights_version(model_version),
        "--skip-if-exists",
        "--no-validate-conversion",
    ]
    print("运行 microsoft/mlvc convert.py …")
    _run(cmd, cwd=mlvc_dir / "video")
    return find_exported_onnx(out)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="调用 microsoft/mlvc convert.py 导出 ONNX（generic）")
    p.add_argument("--mlvc-dir", type=Path, default=None, help="MLVC 源码目录（默认 .build/deps/mlvc）")
    p.add_argument("--data-dir", type=Path, default=None, help="权重与测试数据（默认 .build/deps/mlvc-data）")
    p.add_argument(
        "--weights-path",
        type=Path,
        default=None,
        help="覆盖 checkpoint 路径（MLVC-S 必填）",
    )
    p.add_argument("--model-version", default=DEFAULT_MODEL_VERSION)
    p.add_argument("--weights-version", default=None, help="覆盖 checkpoint 版本名（默认随 --model-version）")
    p.add_argument("--model-width", type=int, default=DEFAULT_WIDTH)
    p.add_argument("--model-height", type=int, default=DEFAULT_HEIGHT)
    p.add_argument("--target-device", default="generic")
    p.add_argument("--frame-count", type=int, default=2, help="convert.py tracing 帧数（默认 2）")
    p.add_argument("--output-path", type=Path, default=None)
    p.add_argument("--force", action="store_true", help="即使已有 ONNX 也重新导出")
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    path = export_onnx(
        mlvc_dir=args.mlvc_dir,
        data_dir=args.data_dir,
        weights_path=args.weights_path,
        model_version=args.model_version,
        weights_version=args.weights_version,
        width=args.model_width,
        height=args.model_height,
        target_device=args.target_device,
        frame_count=args.frame_count,
        output_path=args.output_path,
        skip_if_exists=not args.force,
    )
    print(f"ONNX 目录: {path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OnnxExportError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
