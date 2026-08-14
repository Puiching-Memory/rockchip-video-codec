#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""浅克隆 microsoft/mlvc 并调用 ``convert.py export``，产出 RKNN 转换用的 ONNX + PMF JSON。

不把上游训练代码 vendoring 进本仓。检出与权重落在 ``.build/deps/``。
"""

from __future__ import annotations

import argparse
import hashlib
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
DEFAULT_MODEL_VERSION = "dmc61sbr_reglu"
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


def patch_mlvc_pyproject(text: str) -> str:
    """让当前平台能 uv lock：只解析本机 environment，并去掉 aarch64 上没有的包。"""
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

    sources = text.split("[tool.uv.sources]", 1)[-1] if "[tool.uv.sources]" in text else ""
    if sys.platform == "linux" and platform.machine() == "aarch64" and "aarch64" not in sources:
        m = re.search(
            r'^([ \t]*)\{ index = "pytorch-cpu", marker = "sys_platform == \'win32\' and platform_machine != \'AMD64\'" \},[ \t]*$',
            text,
            re.M,
        )
        if m:
            indent = m.group(1)
            extra = (
                f'{indent}{{ index = "pytorch-cpu", '
                f'marker = "sys_platform == \'linux\' and platform_machine == \'aarch64\'" }},'
            )
            text = text.replace(m.group(0), m.group(0) + "\n" + extra, 1)
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


def patch_mlvc_checkout(mlvc_dir: Path) -> None:
    """让当前平台能 uv sync，并优先用本地权重/测试 YUV（不强制 Azure）。"""
    pyproject = mlvc_dir / "pyproject.toml"
    orig = pyproject.read_text(encoding="utf-8")
    text = patch_mlvc_pyproject(orig)
    if text != orig:
        pyproject.write_text(text, encoding="utf-8")
        print("已补丁 pyproject.toml（当前平台 uv environment / coremltools）")

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


def ensure_checkpoint(data_dir: Path, weights_path: Path | None) -> Path:
    if weights_path is not None:
        if not weights_path.is_file():
            raise OnnxExportError(f"权重不存在: {weights_path}")
        return weights_path
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


def _venv_ok(mlvc_dir: Path) -> bool:
    py = mlvc_dir / ".venv" / "bin" / "python"
    if not py.is_file():
        return False
    proc = subprocess.run(
        [str(py), "-c", "import torch, onnx, msrtc.rans"],
        capture_output=True,
        text=True,
    )
    return proc.returncode == 0


def ensure_mlvc_venv(mlvc_dir: Path) -> None:
    if _venv_ok(mlvc_dir):
        print(f"使用已有 MLVC venv: {mlvc_dir / '.venv'}")
        return
    _run(["uv", "python", "install", "3.12"])
    _run(
        ["uv", "sync", "--python", "3.12", "--no-dev", "--extra", "onnxruntime"],
        cwd=mlvc_dir,
    )
    rans = mlvc_dir / "packages" / "msrtc_rans"
    if not rans.is_dir():
        raise OnnxExportError(f"缺少 {rans}（microsoft/mlvc 熵编码器）")
    _run(["uv", "pip", "install", str(rans)], cwd=mlvc_dir)
    if not _venv_ok(mlvc_dir):
        raise OnnxExportError("MLVC venv 已创建，但 import torch/onnx/msrtc.rans 失败")


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
    width: int = DEFAULT_WIDTH,
    height: int = DEFAULT_HEIGHT,
    target_device: str = "generic",
    frame_count: int = 2,
    output_path: Path | None = None,
    skip_if_exists: bool = True,
) -> Path:
    if target_device == "qualcomm":
        raise OnnxExportError("不要用 --target-device qualcomm（RKNN 上会 CPU fallback）")
    mlvc_dir = ensure_mlvc_src(mlvc_dir or default_mlvc_dir())
    data_dir = data_dir or default_data_dir()
    data_dir.mkdir(parents=True, exist_ok=True)
    test_data_dir = data_dir / "test-set"
    ckpt = ensure_checkpoint(data_dir, weights_path)
    ensure_test_yuv(test_data_dir)
    ensure_mlvc_venv(mlvc_dir)

    out = output_path or (mlvc_dir / "video" / "output" / "models")
    if skip_if_exists:
        try:
            existing = find_exported_onnx(out)
            if (existing / "MLVCDecoder.onnx").is_file() and (existing / "gaussian_pmf.json").is_file():
                print(f"已有 ONNX，跳过 convert.py: {existing}")
                return existing
        except OnnxExportError:
            pass

    cmd = [
        str(mlvc_dir / ".venv" / "bin" / "python"),
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
        "--weights-version", "mlvc-psnr-v1",
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
    p.add_argument("--weights-path", type=Path, default=None, help="覆盖 checkpoint 路径")
    p.add_argument("--model-version", default=DEFAULT_MODEL_VERSION)
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
