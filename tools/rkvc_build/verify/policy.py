"""High-level package verification policy built on ``verify.elf``.

Guards a staging tree before archive:
  * architecture must match the requested target (reject host-x86 ELF)
  * RPATH/RUNPATH must be relative (``$ORIGIN``) -- rejects absolute build dirs
  * shared libraries must carry a SONAME
  * dynamic interpreter must be the expected glibc loader (for executables)
  * DT_NEEDED closure resolves within the package or the given target sysroot
  * glibc symbol-version baseline: reject any ``GLIBC_x.y.z`` above the floor
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import os
import re

from .elf import ElfFile, read_elf

# AArch64 glibc 2.31 loader path (Ubuntu 20.04 sysroot).
AARCH64_LOADER = "/lib/ld-linux-aarch64.so.1"
GLIBC_FLOOR = (2, 31, 0)

EXPECTED_MACHINE = {
    "linux-aarch64-glibc231": ("AArch64", "aarch64"),
    "linux-aarch64": ("AArch64", "aarch64"),
    "linux-armhf": ("ARM", "arm"),
    "linux-x86_64": ("x86-64", "x86-64"),
}

TRIPLET = {"aarch64": "aarch64-linux-gnu", "armhf": "arm-linux-gnueabihf"}

_VERSION_RE = re.compile(r"^GLIBC_([0-9]+)\.([0-9]+)(?:\.([0-9]+))?$")

# Libraries supplied by the target runtime (glibc base + toolchain), never
# bundled in the package per the plan ("package does not carry glibc, dynamic
# loader, libpthread, libdl").  They are satisfied by the target rootfs, so an
# unresolved DT_NEEDED on one of these is not a packaging violation.
_BASE_LIBS = frozenset({
    "libc.so.6", "libpthread.so.0", "libdl.so.2", "libm.so.6", "librt.so.1",
    "libresolv.so.2", "libnsl.so.1", "libutil.so.1", "libgcc_s.so.1",
    "libstdc++.so.6", "ld-linux-aarch64.so.1", "ld-linux-armhf.so.3",
    "ld-linux-x86-64.so.2",
})


def _is_base_lib(name: str) -> bool:
    return name in _BASE_LIBS or name.startswith("libnss_")


@dataclass
class Violation:
    path: str
    kind: str
    detail: str

    def format(self) -> str:
        return f"{self.kind}: {self.path}: {self.detail}"


@dataclass
class VerifyReport:
    target: str
    elf_count: int = 0
    violations: list[Violation] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.violations

    def format(self) -> str:
        lines: list[str] = []
        for v in self.violations:
            lines.append("error: " + v.format())
        for n in self.notes:
            lines.append("note: " + n)
        if not self.violations:
            lines.append(
                f"OK: {self.elf_count} ELF files passed {self.target} checks")
        return "\n".join(lines)


def _glibc_tuple(name: str) -> tuple[int, int, int] | None:
    m = _VERSION_RE.match(name)
    if not m:
        return None
    major, minor, micro = m.groups()
    return (int(major), int(minor), int(micro or 0))


def _populate_package_libs(package: Path) -> dict[str, Path]:
    """Map file name -> resolved ELF path for files under ``lib/``."""
    libs: dict[str, Path] = {}
    lib_dir = package / "lib"
    if lib_dir.is_dir():
        for entry in lib_dir.rglob("*"):
            if not (entry.is_file() or entry.is_symlink()):
                continue
            try:
                resolved = entry.resolve(strict=True)
            except OSError:
                continue
            if _is_elf(resolved):
                libs[entry.name] = resolved
    return libs


def _is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as fh:
            return fh.read(4) == b"\x7fELF"
    except OSError:
        return False


def _iter_elfs(package: Path) -> list[Path]:
    """All ELF files under the staging tree (bin/, lib/, examples/)."""
    roots = [package / "bin", package / "lib", package / "examples"]
    found: list[Path] = []
    for root in roots:
        if not root.is_dir():
            continue
        for entry in root.rglob("*"):
            if entry.is_symlink():
                try:
                    entry = entry.resolve(strict=True)
                except OSError:
                    continue
            if entry.is_file() and _is_elf(entry):
                found.append(entry)
    return found


def _sysroot_dirs(sysroot: Path, arch: str) -> list[Path]:
    triplet = TRIPLET.get(arch, "")
    dirs: list[Path] = []
    if sysroot:
        if triplet:
            dirs += [sysroot / "lib" / triplet, sysroot / "usr/lib" / triplet,
                     sysroot / "usr" / triplet / "lib"]
        dirs += [sysroot / "lib", sysroot / "usr/lib"]
    return dirs


def verify_package(package: Path,
                   target: str,
                   sysroot: Path | None = None,
                   max_glibc: tuple[int, int, int] = GLIBC_FLOOR) -> VerifyReport:
    """Validate a staging tree against the requested ``target``.

    ``target`` is a string like ``linux-aarch64-glibc231`` that the caller has
    already decided it can honour; this function only interprets it for the
    machine/loader expectations.  ``sysroot`` is the immutable target sysroot
    used to resolve external DT_NEEDED entries so we do not trust the host.
    """
    report = VerifyReport(target=target)
    expected_machine, arch = EXPECTED_MACHINE.get(
        target, (None, None))
    if expected_machine is None:
        raise ValueError(f"unknown target: {target}")

    package = package.resolve()
    package_libs = _populate_package_libs(package)
    sysroot_dirs = _sysroot_dirs(sysroot, arch) if sysroot else []

    elfs = _iter_elfs(package)
    report.elf_count = len(elfs)

    checked: set[Path] = set()
    queue: list[Path] = list(elfs)
    while queue:
        raw = queue.pop(0)
        # package_libs stores by resolved path; resolve to compare.
        try:
            elf_path = raw.resolve()
        except OSError:
            continue
        if elf_path in checked:
            continue
        checked.add(elf_path)

        try:
            info = read_elf(str(elf_path))
        except Exception as exc:  # noqa: BLE001
            report.violations.append(
                Violation(str(raw), "elf", f"unreadable: {exc}"))
            continue

        rel = _rel(package, raw)

        # ---- architecture ----
        if info.machine != expected_machine:
            report.violations.append(Violation(
                rel, "arch", f"expected {expected_machine}, got {info.machine}"))

        # ---- glibc baseline ----
        mg = info.max_glibc
        if mg is not None and mg > max_glibc:
            report.violations.append(Violation(
                rel, "glibc",
                f"requires {'.'.join(map(str, mg))} > floor "
                f"{'.'.join(map(str, max_glibc))}"))

        # ---- RPATH / RUNPATH ----
        for tag_name, paths in (("rpath", info.dynamic.rpath),
                                ("runpath", info.dynamic.runpath)):
            for p in paths:
                if p and not (p.startswith("$ORIGIN")
                              or p.startswith("${ORIGIN}") or p == "."):
                    report.violations.append(Violation(
                        rel, "rpath",
                        f"non-relative {tag_name} entry {p!r}"))

        # ---- interpreter ----
        if info.interpreter is not None:
            if target.startswith("linux-aarch64") and \
                    info.interpreter != AARCH64_LOADER:
                report.violations.append(Violation(
                    rel, "interpreter",
                    f"unexpected {info.interpreter!r}, want {AARCH64_LOADER!r}"))

        # ---- writable-exec ----
        if info.load_wx:
            report.violations.append(Violation(
                rel, "w-x", "segment is both writable and executable"))
        if info.stack_exec:
            report.violations.append(Violation(
                rel, "execstack", "executable stack"))

        # ---- SONAME for shared libraries ----
        name = raw.name
        if (name.endswith(".so") or ".so." in name) and not info.dynamic.soname:
            report.violations.append(Violation(
                rel, "soname", "shared object without SONAME"))

        # ---- DT_NEEDED closure ----
        for needed in info.dynamic.needed:
            if _is_base_lib(needed):
                continue  # provided by the target glibc/toolchain runtime
            bundled = package_libs.get(needed)
            if bundled is not None:
                if bundled not in checked:
                    queue.append(bundled)
                continue
            if not _resolve_system(needed, sysroot_dirs):
                report.violations.append(Violation(
                    rel, "dep", f"unresolved DT_NEEDED {needed}"))

        # ---- permissions ----
        try:
            mode = elf_path.stat().st_mode
        except OSError:
            mode = 0
        if mode & 0o002:  # world-writable
            report.violations.append(Violation(
                rel, "perm", "file is world-writable"))

    # de-duplicate violations per (path, kind, detail)
    seen: set[tuple[str, str, str]] = set()
    uniq: list[Violation] = []
    for v in report.violations:
        key = (v.path, v.kind, v.detail)
        if key in seen:
            continue
        seen.add(key)
        uniq.append(v)
    report.violations = uniq
    return report


def _rel(package: Path, path: Path) -> str:
    try:
        return str(path.resolve().relative_to(package))
    except ValueError:
        return str(path)


def _resolve_system(name: str, dirs: list[Path]) -> Path | None:
    for directory in dirs:
        candidate = directory / name
        if candidate.exists():
            return candidate
    return None
