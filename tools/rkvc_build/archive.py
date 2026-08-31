"""Deterministic tar.gz archive writer.

Archive bytes must be reproducible: entries sorted by name, uid/gid zeroed,
mtimes fixed to the package version timestamp (0 = epoch), gzip header mtime
zeroed.  File permission bits are preserved (the verifier audits them).
"""

from __future__ import annotations

import gzip
import os
from pathlib import Path
import tarfile


def create_deterministic_tar_gz(source_dir: Path, out_path: Path,
                                arcname: str) -> Path:
    source_dir = source_dir.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    entries = sorted(
        (p for p in source_dir.rglob("*")),
        key=lambda p: p.relative_to(source_dir).as_posix(),
    )

    def _info(path: Path) -> tarfile.TarInfo:
        rel = path.relative_to(source_dir).as_posix()
        info = tarfile.TarInfo(f"{arcname}/{rel}")
        st = path.lstat()
        info.mode = st.st_mode & 0o7777
        info.mtime = 0
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        if path.is_symlink():
            info.type = tarfile.SYMTYPE
            info.linkname = os.readlink(path)
        elif path.is_dir():
            info.type = tarfile.DIRTYPE
        else:
            info.type = tarfile.REGTYPE
            info.size = st.st_size
        return info

    with out_path.open("wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w",
                              format=tarfile.PAX_FORMAT) as tar:
                # 顶层目录条目
                top = tarfile.TarInfo(arcname)
                top.type = tarfile.DIRTYPE
                top.mode = 0o755
                top.mtime = 0
                tar.addfile(top)
                for path in entries:
                    info = _info(path)
                    if info.isreg():
                        with path.open("rb") as fh:
                            tar.addfile(info, fh)
                    else:
                        tar.addfile(info)
    return out_path
