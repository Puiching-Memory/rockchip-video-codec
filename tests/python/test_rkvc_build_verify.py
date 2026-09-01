#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Tests for the rkvc_build package verifier (stdlib only).

Run directly or through discovery:

    python3 tests/python/test_rkvc_build_verify.py
    python3 -m unittest discover -s tests/python -p 'test_*.py'

The fixtures are real ELF shared objects compiled in a temporary directory.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from rkvc_build.verify import policy  # noqa: E402
from rkvc_build.verify.elf import read_elf  # noqa: E402


def _compile_so(
    output: Path,
    *,
    source: str = "int rkvc_payload(void){return 42;}\n",
    link_flags: list[str] | None = None,
) -> None:
    source_path = output.with_suffix(".c")
    source_path.write_text(source, encoding="utf-8")
    command = [
        os.environ.get("CC", "cc"),
        "-shared",
        "-fPIC",
        "-o",
        str(output),
        str(source_path),
    ]
    if link_flags:
        command.extend(link_flags)
    subprocess.run(command, check=True)


@unittest.skipUnless(sys.platform.startswith("linux"),
                     "ELF verifier tests require Linux")
class PackageVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._temporary_directory = tempfile.TemporaryDirectory()
        cls.base = Path(cls._temporary_directory.name)
        cls.package = cls.base / "package"
        library_dir = cls.package / "lib"
        library_dir.mkdir(parents=True)

        _compile_so(
            library_dir / "libpayload.so.1",
            link_flags=["-Wl,-soname,libpayload.so.1"],
        )
        _compile_so(
            library_dir / "libbadrpath.so.1",
            link_flags=[
                "-Wl,-soname,libbadrpath.so.1",
                "-Wl,-rpath,/opt/rkvc",
            ],
        )
        _compile_so(
            library_dir / "libgoodrpath.so.1",
            link_flags=[
                "-Wl,-soname,libgoodrpath.so.1",
                "-Wl,-rpath,$ORIGIN",
            ],
        )
        cls.report = policy.verify_package(
            cls.package, "linux-aarch64-glibc231"
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls._temporary_directory.cleanup()

    def test_rejects_absolute_rpath(self) -> None:
        self.assertTrue(
            any(v.kind == "rpath" and "libbadrpath" in v.path
                for v in self.report.violations)
        )

    def test_accepts_origin_rpath(self) -> None:
        self.assertFalse(
            any(v.kind == "rpath" and "libgoodrpath" in v.path
                for v in self.report.violations)
        )

    def test_accepts_valid_soname(self) -> None:
        self.assertFalse(
            any(v.kind == "soname" for v in self.report.violations)
        )

    def test_rejects_wrong_architecture(self) -> None:
        self.assertTrue(
            any(v.kind == "arch" for v in self.report.violations)
        )

    def test_rejects_newer_glibc(self) -> None:
        candidates = (Path("/usr/bin/python3"), Path(sys.executable))
        newer_elf = None
        for candidate in candidates:
            if not candidate.exists():
                continue
            info = read_elf(str(candidate))
            if info.max_glibc and info.max_glibc > (2, 31, 0):
                newer_elf = candidate
                break
        if newer_elf is None:
            self.skipTest("no host ELF requires glibc newer than 2.31")

        package = self.base / "new-glibc"
        (package / "bin").mkdir(parents=True)
        (package / "bin" / "newer-glibc").symlink_to(newer_elf)
        report = policy.verify_package(package, "linux-aarch64-glibc231")
        self.assertTrue(any(v.kind == "glibc" for v in report.violations))

    def test_rejects_unresolved_dependency(self) -> None:
        package = self.base / "missing-dependency"
        library_dir = package / "lib"
        library_dir.mkdir(parents=True)
        dependency = library_dir / "libfixture_dependency.so"
        _compile_so(
            dependency,
            source="int fixture_dependency(void){return 42;}\n",
            link_flags=["-Wl,-soname,libfixture_dependency.so"],
        )
        _compile_so(
            library_dir / "libconsumer.so.1",
            source=(
                "extern int fixture_dependency(void);\n"
                "int rkvc_payload(void){return fixture_dependency();}\n"
            ),
            link_flags=[
                f"-L{library_dir}",
                "-lfixture_dependency",
                "-Wl,-soname,libconsumer.so.1",
                "-Wl,-rpath,$ORIGIN",
            ],
        )
        dependency.unlink()

        report = policy.verify_package(package, "linux-aarch64-glibc231")
        self.assertTrue(
            any(v.kind == "dep" and "libfixture_dependency.so" in v.detail
                for v in report.violations)
        )


if __name__ == "__main__":
    unittest.main()
