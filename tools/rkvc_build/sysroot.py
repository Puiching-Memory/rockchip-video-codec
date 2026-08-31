"""Pinned Ubuntu 20.04 (focal) AArch64 sysroot for the glibc 2.31 baseline.

The sysroot is a *host-side build input*, never shipped in the package.
Package versions and SHA-256 digests are pinned in a lockfile committed to
the repo (``sysroot.focal-arm64.lock.json``); without a lockfile the latest
focal/focal-updates versions are resolved once and then frozen.

Stdlib only: urllib + lzma + json.  .deb extraction uses host ``ar``/``tar``.
"""

from __future__ import annotations

import json
import lzma
import os
from pathlib import Path
import subprocess
import urllib.request

MIRROR = os.environ.get("RKVC_SYSROOT_MIRROR",
                        "http://ports.ubuntu.com/ubuntu-ports")
RELEASE = "focal"
SUITES = ("focal", "focal-updates")
ARCH = "arm64"

# Binary packages forming the minimal compile+link sysroot for the glibc 2.31
# baseline.  Anything beyond this set must be justified by verifier failures.
PACKAGES = ("libc6", "libc6-dev", "linux-libc-dev", "libgcc-s1")

LOCKFILE = Path(__file__).resolve().parent / "sysroot.focal-arm64.lock.json"


class SysrootError(RuntimeError):
    pass


def _fetch_packages_index(suite: str) -> dict[str, dict]:
    """Return {package: {version, filename, sha256}} for one suite."""
    url = f"{MIRROR}/dists/{suite}/main/binary-{ARCH}/Packages.xz"
    with urllib.request.urlopen(url, timeout=60) as resp:
        raw = lzma.decompress(resp.read()).decode("utf-8", "replace")
    out: dict[str, dict] = {}
    stanza: dict[str, str] = {}
    for line in raw.splitlines() + [""]:
        if not line:
            if stanza.get("Package") in PACKAGES:
                out[stanza["Package"]] = {
                    "version": stanza["Version"],
                    "filename": stanza["Filename"],
                    "sha256": stanza["SHA256"],
                }
            stanza = {}
            continue
        if line[0].isspace():
            continue
        key, _, value = line.partition(":")
        stanza[key.strip()] = value.strip()
    return out


def _newer(candidate: dict, current: dict | None) -> bool:
    if current is None:
        return True
    rc = subprocess.run(
        ["dpkg", "--compare-versions", candidate["version"], "gt",
         current["version"]], check=False).returncode
    return rc == 0


def resolve_packages() -> dict[str, dict]:
    """Resolve the newest wanted package across all suites."""
    resolved: dict[str, dict] = {}
    for suite in SUITES:
        index = _fetch_packages_index(suite)
        for name in PACKAGES:
            cand = index.get(name)
            if cand and _newer(cand, resolved.get(name)):
                resolved[name] = cand
    missing = [n for n in PACKAGES if n not in resolved]
    if missing:
        raise SysrootError(f"packages not found in index: {', '.join(missing)}")
    return resolved


def load_lock() -> dict | None:
    if not LOCKFILE.exists():
        return None
    return json.loads(LOCKFILE.read_text(encoding="utf-8"))


def save_lock(packages: dict[str, dict]) -> None:
    payload = {
        "release": RELEASE,
        "arch": ARCH,
        "mirror": MIRROR,
        "packages": [
            {"name": name, **packages[name]} for name in sorted(packages)
        ],
    }
    LOCKFILE.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _download(url: str, dest: Path, sha256: str) -> None:
    import hashlib

    if dest.exists():
        digest = hashlib.sha256(dest.read_bytes()).hexdigest()
        if digest == sha256:
            return
        dest.unlink()
    dest.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=300) as resp, \
            dest.open("wb") as fh:
        while True:
            chunk = resp.read(1 << 20)
            if not chunk:
                break
            fh.write(chunk)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    if digest != sha256:
        dest.unlink()
        raise SysrootError(
            f"sha256 mismatch for {url}: got {digest}, want {sha256}")


def _extract_deb(deb: Path, dest: Path) -> None:
    work = deb.parent / f".{deb.stem}.extract"
    if work.exists():
        subprocess.run(["rm", "-rf", str(work)], check=True)
    work.mkdir(parents=True)
    subprocess.run(["ar", "x", str(deb.resolve())], cwd=work, check=True)
    data = next((p for p in work.iterdir() if p.name.startswith("data.tar")),
                None)
    if data is None:
        raise SysrootError(f"{deb.name}: no data.tar member")
    subprocess.run(["tar", "-xf", str(data), "-C", str(dest)], check=True)
    subprocess.run(["rm", "-rf", str(work)], check=True)


def _fix_absolute_symlinks(root: Path) -> int:
    """Rewrite absolute symlinks relative to the sysroot.

    Absolute targets (e.g. /lib/aarch64-linux-gnu/libc.so.6) would otherwise
    resolve against the *host* filesystem during cross compilation.
    """
    fixed = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = Path(dirpath) / name
            if not path.is_symlink():
                continue
            target = os.readlink(path)
            if not target.startswith("/"):
                continue
            rel = os.path.relpath(root / target.lstrip("/"), path.parent)
            path.unlink()
            path.symlink_to(rel)
            fixed += 1
    return fixed


def ensure_sysroot(dest: Path, refresh: bool = False) -> Path:
    """Download + extract the pinned sysroot into *dest*; return the path."""
    lock = None if refresh else load_lock()
    if lock is None:
        packages = resolve_packages()
        save_lock(packages)
    else:
        packages = {p["name"]: {k: p[k] for k in ("version", "filename",
                                                  "sha256")}
                    for p in lock["packages"]}

    deb_dir = dest.parent / "debs"
    dest.mkdir(parents=True, exist_ok=True)
    for name in PACKAGES:
        meta = packages[name]
        url = f"{MIRROR}/{meta['filename']}"
        deb = deb_dir / Path(meta["filename"]).name
        _download(url, deb, meta["sha256"])
        stamp = dest / f".installed-{name}-{meta['version']}"
        if stamp.exists():
            continue
        _extract_deb(deb, dest)
        stamp.touch()

    fixed = _fix_absolute_symlinks(dest)
    # focal 尚未完全 usrmerge：运行时库在 /lib，链接器脚本在 /usr/lib。
    libc = dest / "lib/aarch64-linux-gnu/libc.so.6"
    if not libc.exists():
        raise SysrootError(f"sysroot incomplete: {libc} missing")
    print(f"sysroot: {dest} ({len(PACKAGES)} packages, "
          f"{fixed} absolute symlinks rewritten)")
    return dest
