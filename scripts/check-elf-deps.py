#!/usr/bin/env python3
"""Inspect a portable package's ELF architecture and DT_NEEDED closure.

Unlike ldd, this never executes the inspected program, so it is safe and works
when an x86 host is assembling an AArch64 package.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


class CheckError(RuntimeError):
    pass


NEEDED_RE = re.compile(r"\(NEEDED\).*\[([^]]+)]")
MACHINE_RE = re.compile(r"^\s*Machine:\s*(.+?)\s*$", re.MULTILINE)


def readelf(path: Path, *args: str) -> str:
    proc = subprocess.run(
        ["readelf", *args, str(path)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        raise CheckError(f"readelf failed for {path}: {proc.stderr.strip()}")
    return proc.stdout


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as stream:
            return stream.read(4) == b"\x7fELF"
    except OSError:
        return False


def machine(path: Path) -> str:
    match = MACHINE_RE.search(readelf(path, "-h"))
    if not match:
        raise CheckError(f"cannot determine ELF machine: {path}")
    return match.group(1)


def needed(path: Path) -> list[str]:
    return NEEDED_RE.findall(readelf(path, "-d"))


def target_search_dirs(sysroot: Path | None, target: str) -> list[Path]:
    triplet = {
        "aarch64": "aarch64-linux-gnu",
        "armhf": "arm-linux-gnueabihf",
    }.get(target, "")
    roots = [sysroot] if sysroot else [Path("/")]
    dirs: list[Path] = []
    for root in roots:
        assert root is not None
        if triplet:
            dirs.extend([
                root / "lib" / triplet,
                root / "usr/lib" / triplet,
                root / "usr" / triplet / "lib",
            ])
        dirs.extend([root / "lib", root / "usr/lib"])

    # Debian/Ubuntu cross toolchains conventionally use /usr/<triplet> as the
    # QEMU loader prefix, while multiarch packages still install shared objects
    # in /lib/<triplet> and /usr/lib/<triplet>.  Those directories are part of
    # the same target runtime, even though they sit outside QEMU's -L prefix.
    if sysroot is not None and triplet:
        try:
            debian_cross_root = (Path("/usr") / triplet).resolve()
            if sysroot.resolve() == debian_cross_root:
                dirs.extend([Path("/lib") / triplet, Path("/usr/lib") / triplet])
        except OSError:
            pass
    return dirs


def resolve_system(name: str, dirs: list[Path]) -> Path | None:
    for directory in dirs:
        path = directory / name
        if path.exists():
            return path
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--target", choices=("aarch64", "armhf"), required=True)
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--require-bundled", action="append", default=[])
    args = parser.parse_args()

    package = args.package.resolve()
    lib_dir = package / "lib"
    if not lib_dir.is_dir():
        parser.error(f"package has no lib directory: {lib_dir}")

    package_libs: dict[str, Path] = {}
    for entry in lib_dir.iterdir():
        if entry.is_file() or entry.is_symlink():
            try:
                resolved = entry.resolve(strict=True)
            except OSError:
                continue
            if is_elf(resolved):
                package_libs[entry.name] = resolved

    expected_machine = {
        "aarch64": "AArch64",
        "armhf": "ARM",
    }[args.target]
    roots = [
        path for directory in (package / "bin", package / "examples/bin")
        if directory.is_dir()
        for path in directory.iterdir()
        if path.is_file() and is_elf(path)
    ]
    roots.extend(sorted(set(package_libs.values())))
    if not roots:
        raise CheckError("package contains no ELF binaries")

    errors: list[str] = []
    system_dirs = target_search_dirs(args.sysroot, args.target)
    checked: set[Path] = set()
    queue = list(dict.fromkeys(roots))
    while queue:
        elf = queue.pop(0).resolve()
        if elf in checked:
            continue
        checked.add(elf)
        actual_machine = machine(elf)
        if expected_machine not in actual_machine:
            errors.append(
                f"wrong architecture: {elf.relative_to(package)}: {actual_machine}"
            )
        for name in needed(elf):
            bundled = package_libs.get(name)
            if bundled is not None:
                queue.append(bundled)
            elif resolve_system(name, system_dirs) is None:
                errors.append(
                    f"unresolved target dependency: {elf.relative_to(package)} -> {name}"
                )

    for prefix in args.require_bundled:
        if not any(name == prefix or name.startswith(prefix + ".")
                   for name in package_libs):
            errors.append(f"required bundled library missing: {prefix}")

    if errors:
        for error in sorted(set(errors)):
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        f"OK: {len(checked)} {expected_machine} ELF files; "
        "all DT_NEEDED entries resolve from package or target sysroot"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CheckError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
