"""Rockchip MPP dependency adapter (git submodule, pinned by commit).

Cross-builds ``third_party/mpp`` for the release target with the same
toolchain/sysroot as the core library.  The commit recorded by the git
submodule is the version pin; the adapter never tracks a moving branch.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

from .base import AdapterResult, Dependency, DependencyAdapter

_REPO_ROOT = Path(__file__).resolve().parents[3]
_SUBMODULE = _REPO_ROOT / "third_party" / "mpp"
_TOOLCHAIN = _REPO_ROOT / "cmake" / "toolchains" / "aarch64-linux-gnu.cmake"


def _submodule_commit() -> str:
    out = subprocess.run(
        ["git", "-C", str(_REPO_ROOT), "submodule", "status", "--",
         "third_party/mpp"],
        check=True, capture_output=True, text=True).stdout.strip()
    # 形如 " <sha> third_party/mpp (heads/main)"；前缀 +/- 表示未检出/偏离
    return out.split()[0].lstrip("-+") if out else "unknown"


class MppAdapter(DependencyAdapter):
    kind = "mpp"

    def prefix(self, target_prefix: Path) -> Path:
        return target_prefix / "mpp"

    def probe(self, target) -> Dependency | None:
        if target.arch != "aarch64":
            return None
        if not (_SUBMODULE / "CMakeLists.txt").is_file():
            return None
        return Dependency(name="rockchip-mpp", version=_submodule_commit(),
                          source="submodule", digest=_submodule_commit(),
                          path="third_party/mpp")

    def fetch(self, dep: Dependency) -> AdapterResult:
        if not (_SUBMODULE / "CMakeLists.txt").is_file():
            return AdapterResult(
                False, "third_party/mpp submodule not checked out "
                "(git submodule update --init third_party/mpp)")
        return AdapterResult(True, "ok")

    def build(self, dep: Dependency, host_prefix: Path,
              target_prefix: Path) -> AdapterResult:
        prefix = self.prefix(target_prefix)
        lib = prefix / "lib" / "librockchip_mpp.so"
        stamp = prefix / ".rkvc-complete"
        if lib.is_file() and stamp.is_file():
            self.log.info("mpp: cached build reused")
            return AdapterResult(True, "cached", [lib])

        build_dir = self.work / "deps" / "mpp-build"
        env = dict(os.environ)
        sysroot = env.get("RKVC_SYSROOT")
        if not sysroot:
            return AdapterResult(False, "RKVC_SYSROOT not set by pipeline")

        cmd = [
            "cmake", "-S", str(_SUBMODULE), "-B", str(build_dir),
            "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={_TOOLCHAIN}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_TEST=OFF",
            f"-DCMAKE_INSTALL_PREFIX={prefix}",
        ]
        self.log.info(f"mpp: configure {dep.version[:12]}")
        subprocess.run(cmd, check=True, env=env)
        subprocess.run(["cmake", "--build", str(build_dir),
                        "-j", str(os.cpu_count() or 4)],
                       check=True, env=env)
        subprocess.run(["cmake", "--install", str(build_dir)],
                       check=True, env=env)
        stamp.touch()
        return AdapterResult(True, "built", [lib])

    def install(self, dep: Dependency, staging: Path) -> AdapterResult:
        # 运行库由 cmake_stage 在 cmake --install 之后复制进包根 lib/，
        # 与后端 DSO 的 $ORIGIN/../.. RPATH 对齐。
        return AdapterResult(True, "deferred to cmake_stage")
