"""Tests for the explicit, pinned RKNN Runtime SDK adapter."""

from __future__ import annotations

import pathlib
import tempfile
import types
import unittest
from unittest import mock

from tools.rkvc_build.adapters.rknn import RknnAdapter
from tools.rkvc_build import cmake_stage
from tools.rkvc_build.sbom import aggregate_legal, write_sbom


class _Logger:
    def info(self, message: str) -> None:
        del message


class RknnAdapterTests(unittest.TestCase):
    def test_stages_explicit_sdk_without_host_lookup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            sdk = root / "sdk"
            (sdk / "include").mkdir(parents=True)
            (sdk / "lib").mkdir()
            (sdk / "include" / "rknn_api.h").write_text("/* pinned */\n")
            (sdk / "lib" / "librknnrt.so.2").write_bytes(b"ELF-pinned")
            (sdk / "lib" / "librknnrt.so").symlink_to("librknnrt.so.2")
            license_file = root / "RKNN-LICENSE"
            license_file.write_text("redistribution evidence\n")

            adapter = RknnAdapter(root / "work", _Logger(), sdk, license_file)
            dep = adapter.probe(types.SimpleNamespace(arch="aarch64"))
            self.assertIsNotNone(dep)
            self.assertEqual(dep.source, "explicit-sdk-prefix")
            self.assertTrue(adapter.fetch(dep).ok)
            result = adapter.build(dep, root / "host", root / "target")
            self.assertTrue(result.ok)
            staged = root / "target" / "rknn"
            self.assertEqual(
                (staged / "include" / "rknn_api.h").read_text(),
                "/* pinned */\n",
            )
            self.assertTrue((staged / "lib" / "librknnrt.so").is_symlink())
            self.assertEqual(
                (staged / ".rkvc-complete").read_text().strip(), dep.digest
            )
            self.assertEqual((staged / "LICENSE").read_text(),
                             "redistribution evidence\n")

    def test_rejects_incomplete_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "include").mkdir()
            (root / "include" / "rknn_api.h").write_text("/* no runtime */\n")
            license_file = root / "LICENSE"
            license_file.write_text("evidence\n")
            adapter = RknnAdapter(root / "work", _Logger(), root, license_file)
            self.assertIsNone(adapter.probe(types.SimpleNamespace(arch="aarch64")))

    def test_runtime_is_recorded_in_sbom_and_legal_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            package = pathlib.Path(tmp)
            (package / "lib").mkdir()
            (package / "lib" / "librknnrt.so").write_bytes(b"runtime")
            license_dir = package / "share" / "licenses" / "rknn-runtime"
            license_dir.mkdir(parents=True)
            (license_dir / "LICENSE").write_text("vendor terms\n")

            bom = write_sbom(package, "0.4.0", "linux-aarch64-glibc231")
            self.assertIn('"name": "rknn-runtime"', bom.read_text())
            legal = aggregate_legal(package)
            self.assertEqual(
                (legal / "rknn-runtime.LICENSE").read_text(),
                "vendor terms\n",
            )

    def test_stale_staged_sdk_does_not_enable_backend(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            sysroot = root / "sysroot"
            target_prefix = root / "target"
            sysroot.mkdir()
            (target_prefix / "rknn" / "include").mkdir(parents=True)
            (target_prefix / "rknn" / "lib").mkdir()
            (target_prefix / "rknn" / "include" / "rknn_api.h").touch()
            (target_prefix / "rknn" / "lib" / "librknnrt.so").touch()
            context = types.SimpleNamespace(
                target=types.SimpleNamespace(
                    sysroot=sysroot,
                    name="linux-aarch64-glibc231",
                ),
                target_prefix=target_prefix,
                staging=root / "staging",
                work=root / "work",
                jobs=1,
                rknn_enabled=False,
            )
            with mock.patch.object(cmake_stage, "_run") as run:
                cmake_stage.build_and_install(context, _Logger())
            configure = run.call_args_list[0].args[0]
            self.assertIn("-DRKVC_BUILD_BACKEND_RKNN=OFF", configure)


if __name__ == "__main__":
    unittest.main()
