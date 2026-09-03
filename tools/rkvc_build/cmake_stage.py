"""CMake configure/build/install stage driving.

The orchestrator owns *which* flags define a release build; CMake owns the
targets and the install tree.  No file lists are duplicated here: the staging
content is exactly what ``cmake --install`` produces.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess

SOURCE_ROOT = Path(__file__).resolve().parents[2]
TOOLCHAIN = SOURCE_ROOT / "cmake/toolchains/aarch64-linux-gnu.cmake"


class CMakeStageError(RuntimeError):
    pass


def package_version() -> str:
    """project(VERSION) is the single source of truth for the version."""
    text = (SOURCE_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(\s*rkvc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
                  text)
    if not m:
        raise CMakeStageError("project(VERSION) not found in CMakeLists.txt")
    return m.group(1)


def _run(cmd: list[str], env: dict[str, str] | None, logger) -> None:
    logger.info("+ " + " ".join(str(c) for c in cmd))
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        tail = "\n".join((proc.stdout + proc.stderr).splitlines()[-30:])
        raise CMakeStageError(f"command failed ({proc.returncode}):\n{tail}")


def build_and_install(ctx, logger) -> Path:
    """Cross-build the core package and install it into the staging root.

    Returns the package root inside staging.  The build is incremental by
    CMake/ninja; the sysroot comes from the pinned sysroot stage.
    """
    if not ctx.target.sysroot or not ctx.target.sysroot.exists():
        raise CMakeStageError("target sysroot missing; run sysroot stage first")

    version = package_version()
    pkg_name = f"rkvc-{version}-{ctx.target.name}-portable"
    pkg_root = ctx.staging / pkg_name
    build_dir = ctx.work / "build-target"

    # cmake --install 不清理目标目录；先清空，防止历史构建的过期产物混入包。
    if pkg_root.exists():
        import shutil
        shutil.rmtree(pkg_root)

    env = dict(os.environ)
    env["RKVC_SYSROOT"] = str(ctx.target.sysroot)

    mpp_prefix = ctx.target_prefix / "mpp"
    mpp_available = (mpp_prefix / "lib" / "librockchip_mpp.so").is_file()
    rga_prefix = ctx.target_prefix / "rga"
    rga_available = (rga_prefix / "lib" / "librga.so").is_file()
    rknn_prefix = ctx.target_prefix / "rknn"
    rknn_available = bool(getattr(ctx, "rknn_enabled", False)) and (
        rknn_prefix / "lib" / "librknnrt.so").is_file() and (
        (rknn_prefix / "include" / "rknn_api.h").is_file()
        or (rknn_prefix / "include" / "rknn" / "rknn_api.h").is_file()
    )

    # SVT-AV1 / FFmpeg 容器依赖：软件编码器与 mp4/mkv 等 demux/mux。
    # 二者在项目内预构建（SVT 安装前缀 + ffmpeg-rockchip 源码树内产出的
    # 共享库）；存在即纳入发布包，缺失则按 OFF 构建。
    svt_prefix = SOURCE_ROOT / ".build" / "deps" / "svt-av1-install"
    svt_available = (svt_prefix / "lib" / "libSvtAv1Enc.so").is_file()
    ffmpeg_src = SOURCE_ROOT / "third_party" / "ffmpeg-rockchip"
    ffmpeg_available = all(
        (ffmpeg_src / d / f"lib{lib}.so").is_file()
        for d, lib in (("libavcodec", "avcodec"),
                       ("libavformat", "avformat"),
                       ("libavutil", "avutil"))
    )

    args = [
        "cmake", "-S", str(SOURCE_ROOT), "-B", str(build_dir), "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}",
        "-DCMAKE_BUILD_TYPE=Release",
        # 前缀 /usr 仅为绕开 GNUInstallDirs 对 "/" 的 usr/ 特例化；
        # 安装时由 --prefix 整体替换到包根，配合显式扁平目录得到
        # bin/ lib/ include/ share/ 的可移植布局。
        "-DCMAKE_INSTALL_PREFIX=/usr",
        # 可移植包使用扁平布局（bin/ lib/ include/ share/），不被
        # GNUInstallDirs 的发行版多架构规则改写。
        "-DCMAKE_INSTALL_BINDIR=bin",
        "-DCMAKE_INSTALL_LIBDIR=lib",
        "-DCMAKE_INSTALL_INCLUDEDIR=include",
        "-DCMAKE_INSTALL_DATADIR=share",
        # 交付面：唯一核心库 + 单一 CLI + 已就绪依赖的后端 DSO。
        "-DRKVC_BUILD_SHARED=ON",
        "-DRKVC_BUILD_STATIC=OFF",
        "-DRKVC_BUILD_CLI=ON",
        "-DRKVC_BUILD_TESTS=OFF",
        f"-DRKVC_BUILD_BACKEND_MPP={'ON' if mpp_available else 'OFF'}",
        f"-DRKVC_BUILD_BACKEND_RGA={'ON' if rga_available else 'OFF'}",
        f"-DRKVC_BUILD_BACKEND_RKNN={'ON' if rknn_available else 'OFF'}",
        f"-DRKVC_BUILD_BACKEND_MLVC={'ON' if rknn_available else 'OFF'}",
        f"-DRKVC_BUILD_BACKEND_SVT={'ON' if svt_available else 'OFF'}",
        f"-DRKVC_BUILD_BACKEND_FFMPEG={'ON' if ffmpeg_available else 'OFF'}",
    ]
    if mpp_available:
        args += [
            f"-DMPP_INSTALL_PREFIX={mpp_prefix}",
        ]
    if rga_available:
        args += [
            f"-DRGA_INSTALL_PREFIX={rga_prefix}",
        ]
    if rknn_available:
        args += [
            f"-DRKNN_INSTALL_PREFIX={rknn_prefix}",
        ]
    _run(args, env, logger)

    _run(["cmake", "--build", str(build_dir), "-j", str(ctx.jobs)],
         env, logger)
    _run(["cmake", "--install", str(build_dir), "--prefix", str(pkg_root)],
         env, logger)

    # 厂商运行库随包分发到扁平 lib/（后端 DSO
    # INSTALL_RPATH=$ORIGIN/../..）。是否允许再分发由发布环境负责把关。
    if mpp_available or rga_available or rknn_available \
            or svt_available or ffmpeg_available:
        import shutil
        dst = pkg_root / "lib"
        dst.mkdir(parents=True, exist_ok=True)

        def _copy_runtime(lib_dir: Path, pattern: str) -> None:
            for so in sorted(lib_dir.glob(pattern)):
                target = dst / so.name
                if so.is_symlink():
                    if target.exists():
                        target.unlink()
                    target.symlink_to(os.readlink(so))
                elif so.is_file():
                    shutil.copy2(so, target)

        if mpp_available:
            _copy_runtime(mpp_prefix / "lib", "librockchip_mpp.so*")
        if rga_available:
            _copy_runtime(rga_prefix / "lib", "librga.so*")
        if rknn_available:
            _copy_runtime(rknn_prefix / "lib", "librknnrt.so*")
            license_src = rknn_prefix / "LICENSE"
            if license_src.is_file():
                license_dst = pkg_root / "share" / "licenses" / "rknn-runtime"
                license_dst.mkdir(parents=True, exist_ok=True)
                shutil.copy2(license_src, license_dst / "LICENSE")
        if svt_available:
            _copy_runtime(svt_prefix / "lib", "libSvtAv1Enc.so*")
            for name in ("LICENSE.md", "LICENSE"):
                license_src = SOURCE_ROOT / "third_party" / "SVT-AV1" / name
                if license_src.is_file():
                    license_dst = pkg_root / "share" / "licenses" / "svt-av1"
                    license_dst.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(license_src, license_dst / license_src.name)
                    break
        if ffmpeg_available:
            for d, lib in (("libavcodec", "avcodec"),
                           ("libavformat", "avformat"),
                           ("libavutil", "avutil")):
                _copy_runtime(ffmpeg_src / d, f"lib{lib}.so*")
            for name in ("COPYING.LGPLv2.1", "COPYING.GPLv2"):
                license_src = ffmpeg_src / name
                if license_src.is_file():
                    license_dst = pkg_root / "share" / "licenses" / "ffmpeg"
                    license_dst.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(license_src, license_dst / license_src.name)
    return pkg_root
