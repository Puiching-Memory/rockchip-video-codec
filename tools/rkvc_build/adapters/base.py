"""Dependency adapter protocol.

Each adapter describes one dependency (mpp, svt-av1, ffmpeg, librga, rknn,
libsodium, ...).  Adapters implement ``probe/fetch/build/install`` and are
discovered by directory scan under ``tools/rkvc_build/adapters/`` -- the main
orchestrator never maintains a dependency branch table.

Version pinning is the adapter's responsibility: a git submodule, an
immutable download digest, or a container digest.  Adapters must not depend on
"the latest".
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class Dependency:
    name: str
    version: str
    source: str            # submodule / digest / container
    digest: str            # immutable content digest
    path: str = ""         # reproducible vendored path


@dataclass
class AdapterResult:
    ok: bool
    message: str
    artifacts: list[Any] = field(default_factory=list)


class DependencyAdapter:
    """Base class.  Concrete adapters override the lifecycle methods."""

    kind = "base"

    def __init__(self, work: Any, logger: Any):
        self.work = work
        self.log = logger

    def probe(self, target: Any) -> Dependency | None:
        """Return a pinned ``Dependency`` if this adapter applies to ``target``."""
        raise NotImplementedError

    def fetch(self, dep: Dependency) -> AdapterResult:
        raise NotImplementedError

    def build(self, dep: Dependency, host_prefix: Any,
              target_prefix: Any) -> AdapterResult:
        raise NotImplementedError

    def install(self, dep: Dependency, staging: Any) -> AdapterResult:
        raise NotImplementedError
