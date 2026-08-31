"""QEMU user-mode smoke stage.

QEMU proves loading, CLI wiring and hardware-free paths only.  VPU/RGA/NPU
function and performance remain gated by the hardware lab, never by QEMU.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess


class SmokeError(RuntimeError):
    pass


def qemu_smoke(pkg_root: Path, sysroot: Path | None, logger) -> None:
    qemu = shutil.which("qemu-aarch64")
    if not qemu:
        raise SmokeError("qemu-aarch64 not found on host PATH")

    cli = pkg_root / "bin" / "rkvc"
    if not cli.exists():
        raise SmokeError(f"{cli} missing from staging tree")

    base = [qemu]
    if sysroot:
        base += ["-L", str(sysroot)]

    def _run(*cli_args: str) -> dict:
        proc = subprocess.run(base + [str(cli), *cli_args],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise SmokeError(
                f"rkvc {' '.join(cli_args)} failed ({proc.returncode}): "
                f"{proc.stderr.strip()}")
        try:
            return json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            raise SmokeError(f"rkvc {' '.join(cli_args)} did not emit JSON: "
                             f"{proc.stdout.strip()}") from exc

    version = _run("version", "--json")
    if not version.get("version"):
        raise SmokeError("version payload empty")
    logger.info(f"qemu smoke: rkvc {version['version']} abi "
                f"{version.get('abi')}")

    device = _run("inspect", "device", "--json")
    if device.get("status") != "ok":
        raise SmokeError("inspect device did not report ok")
    logger.info("qemu smoke: inspect device ok")
