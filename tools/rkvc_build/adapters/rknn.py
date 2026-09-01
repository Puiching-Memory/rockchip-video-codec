"""Explicit RKNN Runtime SDK adapter.

RKNN Runtime is not vendored by this repository.  A release operator supplies
an audited SDK prefix with ``--rknn-sdk``; this adapter validates and copies
the exact header/runtime files into the target dependency prefix.  The source
tree is never guessed from a host installation.
"""

from __future__ import annotations

import hashlib
import shutil
from pathlib import Path

from .base import AdapterResult, Dependency, DependencyAdapter


class RknnAdapter(DependencyAdapter):
    kind = "rknn"

    def __init__(self, work, logger, sdk_prefix: Path, license_file: Path):
        super().__init__(work, logger)
        self.sdk_prefix = sdk_prefix.resolve()
        self.license_file = license_file.resolve()

    @staticmethod
    def prefix(target_prefix: Path) -> Path:
        return target_prefix / "rknn"

    def _header(self) -> Path | None:
        for rel in ("include/rknn_api.h", "include/rknn/rknn_api.h"):
            candidate = self.sdk_prefix / rel
            if candidate.is_file():
                return candidate
        return None

    def _runtime_files(self) -> list[Path]:
        return sorted(p for p in (self.sdk_prefix / "lib").glob("librknnrt.so*")
                      if p.is_file() or p.is_symlink())

    def _digest(self, header: Path, runtimes: list[Path]) -> str:
        digest = hashlib.sha256()
        for path in [header, *runtimes]:
            digest.update(str(path.relative_to(self.sdk_prefix)).encode())
            digest.update(b"\0")
            digest.update(path.resolve().read_bytes())
        digest.update(b"license\0")
        digest.update(self.license_file.read_bytes())
        return digest.hexdigest()

    def probe(self, target) -> Dependency | None:
        if target.arch != "aarch64":
            return None
        header = self._header()
        runtimes = self._runtime_files()
        if (header is None or
                not any(p.name == "librknnrt.so" for p in runtimes) or
                not self.license_file.is_file()):
            return None
        digest = self._digest(header, runtimes)
        return Dependency(name="rknn-runtime", version=digest[:12],
                          source="explicit-sdk-prefix", digest=digest,
                          path=str(self.sdk_prefix))

    def fetch(self, dep: Dependency) -> AdapterResult:
        del dep
        if (self._header() is None or not self._runtime_files() or
                not self.license_file.is_file()):
            return AdapterResult(
                False, "RKNN SDK requires rknn_api.h, librknnrt.so and "
                "explicit license evidence")
        return AdapterResult(True, "ok")

    def build(self, dep: Dependency, host_prefix: Path,
              target_prefix: Path) -> AdapterResult:
        del host_prefix
        prefix = self.prefix(target_prefix)
        if prefix.exists():
            shutil.rmtree(prefix)
        shutil.copytree(self.sdk_prefix / "include", prefix / "include",
                        symlinks=True)
        (prefix / "lib").mkdir(parents=True, exist_ok=True)
        for runtime in self._runtime_files():
            target = prefix / "lib" / runtime.name
            if runtime.is_symlink():
                target.symlink_to(runtime.readlink())
            else:
                shutil.copy2(runtime, target)
        shutil.copy2(self.license_file, prefix / "LICENSE")
        (prefix / ".rkvc-complete").write_text(dep.digest + "\n")
        return AdapterResult(True, "staged", [prefix / "lib" / "librknnrt.so"])

    def install(self, dep: Dependency, staging: Path) -> AdapterResult:
        del dep, staging
        return AdapterResult(True, "deferred to cmake_stage")
