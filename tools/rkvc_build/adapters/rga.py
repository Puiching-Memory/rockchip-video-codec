"""Rockchip RGA dependency adapter (git submodule, pinned by commit).

librga 上游只发布头文件与预编译库（无开源内核源码），因此本适配器的
"build" 是确定性的 SDK 前缀装配：从子模块复制 include/ 与目标架构的
librga.so 到 ``.build/deps/rga-install``。子模块提交即版本锚点。
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from .base import AdapterResult, Dependency, DependencyAdapter

_REPO_ROOT = Path(__file__).resolve().parents[3]
_SUBMODULE = _REPO_ROOT / "third_party" / "librga"

# 目标架构 → 子模块预编译目录（Linux/gcc-<toolchain>/）
_ARCH_LIBDIR = {"aarch64": "gcc-aarch64", "armhf": "gcc-armhf"}


def _submodule_commit() -> str:
    out = subprocess.run(
        ["git", "-C", str(_REPO_ROOT), "submodule", "status", "--",
         "third_party/librga"],
        check=True, capture_output=True, text=True).stdout.strip()
    return out.split()[0].lstrip("-+") if out else "unknown"


class RgaAdapter(DependencyAdapter):
    kind = "rga"

    def __init__(self, work, logger):
        super().__init__(work, logger)
        self._arch: str | None = None  # probe() 记录，供 build() 选择预编译库

    def prefix(self, target_prefix: Path) -> Path:
        return target_prefix / "rga"

    def probe(self, target) -> Dependency | None:
        if target.arch not in _ARCH_LIBDIR:
            return None
        libdir = _SUBMODULE / "libs" / "Linux" / _ARCH_LIBDIR[target.arch]
        if not (_SUBMODULE / "include" / "im2d.h").is_file():
            return None
        if not (libdir / "librga.so").is_file():
            return None
        self._arch = target.arch
        return Dependency(name="librga", version=_submodule_commit(),
                          source="submodule", digest=_submodule_commit(),
                          path="third_party/librga")

    def fetch(self, dep: Dependency) -> AdapterResult:
        if not (_SUBMODULE / "include" / "im2d.h").is_file():
            return AdapterResult(
                False, "third_party/librga submodule not checked out "
                "(git submodule update --init third_party/librga)")
        return AdapterResult(True, "ok")

    def build(self, dep: Dependency, host_prefix: Path,
              target_prefix: Path) -> AdapterResult:
        del host_prefix  # 预编译组件：无宿主构建步骤
        if not self._arch:
            return AdapterResult(False, "probe() must run before build()")
        libs = _SUBMODULE / "libs" / "Linux" / _ARCH_LIBDIR[self._arch]
        prefix = self.prefix(target_prefix)
        stamp = prefix / ".rkvc-complete"
        if stamp.is_file() and (prefix / "lib" / "librga.so").is_file():
            self.log.info("rga: cached SDK prefix reused")
            return AdapterResult(True, "cached",
                                 [prefix / "lib" / "librga.so"])

        inc_dst = prefix / "include"
        lib_dst = prefix / "lib"
        if inc_dst.exists():
            shutil.rmtree(inc_dst)
        if lib_dst.exists():
            shutil.rmtree(lib_dst)
        shutil.copytree(_SUBMODULE / "include", inc_dst)
        lib_dst.mkdir(parents=True, exist_ok=True)
        for so in sorted(libs.glob("librga.so*")):
            if so.is_symlink() or so.is_file():
                shutil.copy2(so, lib_dst / so.name)
        stamp.touch()
        self.log.info(f"rga: SDK prefix assembled from {dep.version[:12]}")
        return AdapterResult(True, "assembled", [lib_dst / "librga.so"])

    def install(self, dep: Dependency, staging: Path) -> AdapterResult:
        # 运行库由 cmake_stage 复制进包根 lib/（对齐后端 DSO 的
        # $ORIGIN/../.. RPATH），此处不再搬运。
        return AdapterResult(True, "deferred to cmake_stage")
