"""Digest-based content cache.

Every stage is keyed by a stable digest of its declared inputs (sources,
configuration, host/target identity).  A cache hit means the stage's declared
output already exists with the same inputs, so the orchestration can skip it.
Determinism of the final archive derives from these input digests being
stable.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


class Cache:
    """A flat content-addressed cache under ``.build/cache/``."""

    def __init__(self, root: Path):
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def digest(payload: object) -> str:
        blob = json.dumps(payload, sort_keys=True, default=str).encode("utf-8")
        return hashlib.sha256(blob).hexdigest()

    def _dir(self, key: str) -> Path:
        return self.root / key[:2] / key

    def has(self, key: str) -> bool:
        return self._dir(key).is_dir()

    def save(self, key: str, artifacts: list[Path]) -> None:
        """Copy ``artifacts`` into the cache directory for ``key``."""
        target = self._dir(key)
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True, exist_ok=True)
        for artifact in artifacts:
            if not artifact.exists():
                continue
            dest = target / artifact.name
            if artifact.is_dir():
                shutil.copytree(artifact, dest, symlinks=True)
            else:
                shutil.copy2(artifact, dest, follow_symlinks=False)

    def restore(self, key: str, dest: Path) -> list[Path]:
        """Copy cached artifacts back to ``dest``; return restored paths."""
        src = self._dir(key)
        restored: list[Path] = []
        if not src.is_dir():
            return restored
        dest.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            target = dest / child.name
            if child.is_dir():
                if target.exists():
                    shutil.rmtree(target)
                shutil.copytree(child, target, symlinks=True)
            else:
                shutil.copy2(child, target, follow_symlinks=False)
            restored.append(target)
        return restored
