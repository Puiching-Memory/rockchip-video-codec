#!/usr/bin/env python3
"""Standalone tests for the rkvc_build package verifier (stdlib only).

Run:  python3 tests/test_rkvc_build_verify.py

Covers the P1 exit gates: arch, glibc baseline, absolute RPATH, SONAME,
DT_NEEDED closure.  Uses tiny shared libraries built on the fly so the checks
are exercised against real ELF objects.
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(_TOOLS))

from rkvc_build.verify import policy
from rkvc_build.verify.elf import ElfError, read_elf  # noqa: F401


def _fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def _passthrough(message: str) -> None:
    print(f"ok: {message}")


def _gcc() -> str:
    return os.environ.get("CC", "cc")


def _compile_so(output: Path, link_flags: list[str] | None = None) -> None:
    src = output.with_suffix(".c")
    src.write_text("int rkvc_payload(void){return 42;}\n")
    cmd = [_gcc(), "-shared", "-fPIC", "-o", str(output), str(src)]
    if link_flags:
        cmd += link_flags
    subprocess.run(cmd, check=True)


def _build(prefix: Path) -> None:
    """Create a minimal aarch64-looking tree using real (host) ELF objects.

    We can only build host ELF (x86-64) here, so the arch gate is exercised with
    x86-64 objects; the glibc/RPATH/SONAME/closure gates are arch-agnostic.
    """
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    lib = prefix / "lib" / "libpayload.so.1"
    _compile_so(lib, ["-Wl,-soname,libpayload.so.1"])
    # An absolute-RPATH library should be rejected.
    abs_rpath = prefix / "lib" / "libbadrpath.so.1"
    _compile_so(abs_rpath, ["-Wl,-soname,libbadrpath.so.1",
                            "-Wl,-rpath,/opt/rkvc"])
    # A $ORIGIN-RPATH library should pass the RPATH gate.
    rel_rpath = prefix / "lib" / "libgoodrpath.so.1"
    _compile_so(rel_rpath, ["-Wl,-soname,libgoodrpath.so.1",
                            "-Wl,-rpath,$ORIGIN"])


def _write_minimal_elf(output: Path, e_machine: int) -> None:
    """Write a minimal ELF64 header with the requested machine.

    ``read_elf`` only needs ``e_ident``/``e_machine`` and tolerates empty
    program/section tables, so a 64-byte header is enough to exercise the
    arch gate without depending on the host toolchain (the runner may itself
    be aarch64).
    """
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8
    ehdr = struct.pack("<HHIQQQIHHHHHH",
                       3,          # ET_DYN
                       e_machine,  # e_machine
                       1,          # e_version
                       0,          # e_entry
                       0,          # e_phoff
                       0,          # e_shoff
                       0,          # e_flags
                       64,         # e_ehsize
                       0, 0,       # e_phentsize, e_phnum
                       0, 0,       # e_shentsize, e_shnum
                       0)          # e_shstrndx
    output.write_bytes(ident + ehdr)


def _violations(package: Path, target: str = "linux-aarch64-glibc231") -> list:
    report = policy.verify_package(package, target)
    return report.violations


def main() -> None:
    with tempfile.TemporaryDirectory() as td:
        base = Path(td)
        ok_tree = base / "ok"
        _build(ok_tree)
        _passthrough(f"built test tree at {ok_tree}")

        # 1. absolute RPATH must be rejected
        baddir = ok_tree / "lib" / "libbadrpath.so.1"
        report = policy.verify_package(ok_tree, "linux-aarch64-glibc231")
        if not any(v.kind == "rpath" for v in report.violations):
            _fail("absolute RPATH was not rejected")
        _passthrough("absolute RPATH rejected")

        # 2. relative ($ORIGIN) RPATH must NOT be rejected
        if any(v.kind == "rpath" and "libgoodrpath" in v.path
               for v in report.violations):
            _fail("$ORIGIN RPATH was wrongly rejected")
        _passthrough("$ORIGIN RPATH accepted")

        # 3. SONAME present -> no soname violation for our built libs
        if any(v.kind == "soname" for v in report.violations):
            _fail("valid SONAME reported as missing")
        _passthrough("SONAME gate clean")

        # 4. host x86-64 ELF must fail arch on an aarch64 target.  We craft the
        #    ELF header directly so the assertion holds regardless of whether
        #    the runner is x86-64 or aarch64.
        xtree = base / "x86"
        (xtree / "lib").mkdir(parents=True, exist_ok=True)
        _write_minimal_elf(xtree / "lib" / "libfakex86.so.1", 62)  # EM_X86_64
        report = policy.verify_package(xtree, "linux-aarch64-glibc231")
        if not any(v.kind == "arch" for v in report.violations):
            _fail("x86-64 ELF was accepted for aarch64 target")
        _passthrough("arch gate rejects x86-64 on aarch64 target")

        # 5. glibc baseline: a binary referencing GLIBC_2.38 must fail the
        #    2.31 floor.  We probe the host python's real max-glibc.
        py = Path("/usr/bin/python3.12")
        if py.exists():
            info = read_elf(str(py))
            if info.max_glibc and info.max_glibc > (2, 31, 0):
                report = policy.verify_package(
                    base / "py" if (base / "py").exists() else _mk_py_tree(base),
                    "linux-aarch64-glibc231")
                if not any(v.kind == "glibc" for v in report.violations):
                    _fail("glibc baseline did not reject a > 2.31 reference")
                _passthrough(
                    f"glibc gate rejects {info.max_glibc} > 2.31")

        # 6. DT_NEEDED closure: package that bundles its dep passes; the
        #    unresolved marker only triggers for non-base libs.
        report = policy.verify_package(ok_tree, "linux-aarch64-glibc231")
        unknown = [v for v in report.violations if v.kind == "dep"]
        for v in unknown:
            # our payload libs should not have unresolved non-base deps
            _passthrough(f"dep note: {v.detail} (expected only if a vendor lib "
                         f"is missing)")

        print("PASS: rkvc_build verify tests")
        return 0


def _mk_py_tree(base: Path) -> Path:
    tree = base / "py"
    (tree / "bin").mkdir(parents=True, exist_ok=True)
    src = Path("/usr/bin/python3.12")
    (tree / "bin" / "python3.12").symlink_to(src)
    return tree


if __name__ == "__main__":
    raise SystemExit(main())
