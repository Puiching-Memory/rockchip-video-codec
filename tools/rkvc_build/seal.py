"""Seal stage: manifest + provenance generated from the finished staging tree.

The manifest is audit evidence derived from real install content — it is
never an input to build or runtime routing (no second source of truth).
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _git_revision() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True,
            cwd=Path(__file__).resolve().parents[2]).stdout.strip()
    except Exception:  # noqa: BLE001 - 非 git 环境仅记录为 unknown
        return "unknown"


def seal(pkg_root: Path, target_name: str, version: str) -> Path:
    """Write SHA256SUMS + provenance.json into the package root."""
    files = sorted(p for p in pkg_root.rglob("*") if p.is_file())
    sums = pkg_root / "SHA256SUMS"
    lines = []
    for path in files:
        rel = path.relative_to(pkg_root).as_posix()
        if rel in ("SHA256SUMS", "provenance.json"):
            continue
        lines.append(f"{_sha256(path)}  {rel}")
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8")

    provenance = {
        "package": pkg_root.name,
        "version": version,
        "target": target_name,
        "source_revision": _git_revision(),
        "file_count": len(lines),
    }
    (pkg_root / "provenance.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    return sums
