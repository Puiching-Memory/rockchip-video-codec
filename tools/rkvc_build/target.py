"""Host / target / sysroot model for the release build."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import platform


@dataclass
class Host:
    """The environment running the build (x86_64 host tooling)."""

    machine: str
    system: str
    python: str
    toolchain_dir: Path | None = None

    @classmethod
    def probe(cls) -> "Host":
        return cls(machine=platform.machine(), system=platform.system(),
                   python=platform.python_version())


@dataclass
class Target:
    """A target binary ABI, e.g. ``linux-aarch64-glibc231``.

    The string is a request, not an internal support table: individual Toolkit
    or sysroot steps accept or reject it.
    """

    name: str
    arch: str            # aarch64 / armhf / x86_64
    libc: str            # glibc
    libc_floor: tuple[int, int, int]
    sysroot: Path | None = None
    triple: str = ""

    def __post_init__(self) -> None:
        if not self.triple:
            self.triple = {"aarch64": "aarch64-linux-gnu",
                           "armhf": "arm-linux-gnueabihf"}.get(
                self.arch, self.arch)

    @classmethod
    def parse(cls, name: str, sysroot: Path | None = None) -> "Target":
        """Parse ``linux-aarch64-glibc231`` -> ``linux``/``aarch64``/``glibc231``."""
        arch = "aarch64"
        if "aarch64" in name:
            arch = "aarch64"
        elif "armhf" in name or "arm" in name:
            arch = "armhf"
        elif "x86_64" in name or "amd64" in name or "x86-64" in name:
            arch = "x86_64"

        libc_floor = (2, 31, 0)
        if "glibc" in name:
            for seg in name.split("-"):
                if seg.startswith("glibc") and seg != "glibc":
                    body = seg[len("glibc"):]
                    if "." in body:  # explicit, e.g. glibc2.31
                        nums = [int(p) for p in body.split(".") if p.isdigit()]
                    else:            # short, e.g. glibc231 -> (2, 31, 0)
                        nums = [2, int(body[1:])] if body.startswith("2") \
                            else [int(body[0])]
                    while len(nums) < 3:
                        nums.append(0)
                    libc_floor = (nums[0], nums[1], nums[2])
        return cls(name=name, arch=arch, libc="glibc", libc_floor=libc_floor,
                   sysroot=sysroot)


@dataclass
class BuildContext:
    """Immutable context threaded through every stage."""

    host: Host
    target: Target
    work: Path
    host_prefix: Path
    target_prefix: Path
    staging: Path
    cache: Path
    jobs: int = 1

    @classmethod
    def create(cls, target_name: str, base: Path, jobs: int = 1) -> "BuildContext":
        host = Host.probe()
        target = Target.parse(target_name)
        base = base.resolve()
        work = base / "work"
        host_prefix = base / "host"
        target_prefix = base / "target"
        staging = base / "staging"
        cache = base / "cache"
        return cls(host=host, target=target, work=work, host_prefix=host_prefix,
                   target_prefix=target_prefix, staging=staging, cache=cache,
                   jobs=jobs)
