"""libsodium dependency adapter (git submodule, pinned by commit).

libsodium provides SHA-256 + Ed25519/XSalsa20-Poly1305 for the rkmodel trust
stack and is required by the core library on every target.  The build goes
through ``tools/install-libsodium.sh`` (autotools), cross-compiled for the
release target via ``RKVC_TARGET_ARCH`` so the triplet logic stays in one
place.  The commit recorded by the git submodule is the version pin.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

from .base import AdapterResult, Dependency, DependencyAdapter

_REPO_ROOT = Path(__file__).resolve().parents[3]
_SUBMODULE = _REPO_ROOT / "third_party" / "libsodium"
_SCRIPT = _REPO_ROOT / "tools" / "install-libsodium.sh"


def _submodule_commit() -> str:
    out = subprocess.run(
        ["git", "-C", str(_REPO_ROOT), "submodule", "status", "--",
         "third_party/libsodium"],
        check=True, capture_output=True, text=True).stdout.strip()
    return out.split()[0].lstrip("-+") if out else "unknown"


class SodiumAdapter(DependencyAdapter):
    kind = "libsodium"

    def prefix(self, target_prefix: Path) -> Path:
        return target_prefix / "libsodium"

    def probe(self, target) -> Dependency | None:
        if target.arch != "aarch64":
            return None
        if not (_SUBMODULE / "configure.ac").is_file():
            return None
        return Dependency(name="libsodium", version=_submodule_commit(),
                          source="submodule", digest=_submodule_commit(),
                          path="third_party/libsodium")

    def fetch(self, dep: Dependency) -> AdapterResult:
        if not (_SUBMODULE / "configure.ac").is_file():
            return AdapterResult(
                False, "third_party/libsodium submodule not checked out "
                "(git submodule update --init third_party/libsodium)")
        return AdapterResult(True, "ok")

    def build(self, dep: Dependency, host_prefix: Path,
              target_prefix: Path) -> AdapterResult:
        prefix = self.prefix(target_prefix)
        lib = prefix / "lib" / "libsodium.a"
        stamp = prefix / ".rkvc-complete"
        if lib.is_file() and stamp.is_file():
            self.log.info("libsodium: cached build reused")
            return AdapterResult(True, "cached", [lib])

        env = dict(os.environ)
        env["RKVC_TARGET_ARCH"] = "aarch64"
        env["PREFIX"] = str(prefix)
        env["BUILD_DIR"] = str(self.work / "deps" / "libsodium-build")
        self.log.info(f"libsodium: build {dep.version[:12]} -> {prefix}")
        proc = subprocess.run(["bash", str(_SCRIPT)], env=env,
                              capture_output=True, text=True)
        if proc.returncode != 0 or not lib.is_file():
            tail = "\n".join((proc.stdout + proc.stderr).splitlines()[-20:])
            return AdapterResult(False, f"install-libsodium.sh failed:\n{tail}")
        stamp.touch()
        return AdapterResult(True, "built", [lib])

    def install(self, dep: Dependency, staging: Path) -> AdapterResult:
        # 静态链接进 librkvc，无需随包分发；前缀由 cmake_stage 经
        # -DLIBSODIUM_PREFIX 注入。
        return AdapterResult(True, "static; linked into librkvc")
