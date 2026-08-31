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

    env = dict(os.environ)
    env["RKVC_SYSROOT"] = str(ctx.target.sysroot)

    _run([
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
        # P1 交付面：新引擎核心库 + 单一 CLI；旧库与 Rockchip 依赖在后续
        # 适配器就绪前不进入可移植包。
        "-DRKVC_BUILD_NEW_ENGINE=ON",
        "-DBUILD_SHARED_LIBS=ON",
        "-DRKVC_BUILD_SHARED=OFF",
        "-DRKVC_BUILD_STATIC=OFF",
        "-DRKVC_BUILD_CLI=OFF",
        "-DRKVC_BUILD_EXAMPLES=OFF",
        "-DRKVC_BUILD_TESTS=OFF",
        "-DRKVC_ENABLE_RKNN=OFF",
        "-DRKVC_ENABLE_MLVC=OFF",
    ], env, logger)

    _run(["cmake", "--build", str(build_dir), "-j", str(ctx.jobs)],
         env, logger)
    _run(["cmake", "--install", str(build_dir), "--prefix", str(pkg_root)],
         env, logger)
    return pkg_root
