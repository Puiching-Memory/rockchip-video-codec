#!/usr/bin/env python3
"""加载 bench/config.json，解析路径与 RD 校准表。"""

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional


def _resolve_path(project_root: Path, value: Optional[str]) -> Optional[str]:
    if not value:
        return None
    path = Path(value)
    if path.is_absolute():
        return str(path)
    return str((project_root / path).resolve())


def load_config(config_path: Path, project_root: Path) -> Dict[str, Any]:
    with config_path.open(encoding="utf-8") as f:
        cfg = json.load(f)

    paths = cfg.setdefault("paths", {})
    for key in (
        "ffmpeg",
        "ffprobe",
        "svt_enc",
        "rkvc_build",
        "mpp_lib",
        "ffmpeg_src",
        "svt_prefix",
        "rkvc_sr_model",
        "superres_decode_ffmpeg",
    ):
        if key in paths and paths[key]:
            paths[key] = _resolve_path(project_root, paths[key])
    return cfg


def _dig(cfg: Dict[str, Any], dotted: str) -> Any:
    cur: Any = cfg
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            raise KeyError(dotted)
        cur = cur[part]
    return cur


def lookup_calibration(cfg: Dict[str, Any], table: str, kbps: int) -> int:
    """table 形如 h264.full / svt_av1.low_qp。"""
    parts = table.split(".")
    if len(parts) != 2:
        raise ValueError(f"bad calibration table: {table}")
    family, variant = parts
    tables = cfg.get("calibration", {})
    defaults = tables.get("defaults", {})
    default_key = f"{family}.{variant}"
    default = int(defaults.get(default_key, 0))
    mapping = tables.get(family, {}).get(variant, {})
    key = str(int(kbps))
    if key in mapping:
        return int(mapping[key])
    return default


def validate_config(cfg: Dict[str, Any]) -> None:
    ffmpeg = cfg["paths"]["ffmpeg"]
    ffprobe = cfg["paths"]["ffprobe"]
    for label, path in (("ffmpeg", ffmpeg), ("ffprobe", ffprobe)):
        if not path or not Path(path).is_file():
            raise SystemExit(
                f"[error] {label} 未找到: {path}\n"
                "请先运行: ./scripts/rebuild-ffmpeg-rkmpp.sh"
            )
        if not os_access_executable(path):
            raise SystemExit(f"[error] {label} 不可执行: {path}")


def os_access_executable(path: str) -> bool:
    import os

    return os.path.isfile(path) and os.access(path, os.X_OK)


def defaults_env(cfg: Dict[str, Any]) -> Dict[str, str]:
    clip = cfg["clip"]
    run = cfg["run"]
    svt = cfg["svt"]
    superres = svt.get("superres", {})
    paths = cfg["paths"]
    elementary = clip.get("elementary_mp4", {})

    return {
        "CLIP_SEC": str(clip.get("sec", 4)),
        "CLIP_OFFSET": str(clip.get("offset", "middle")),
        "TARGET_KBPS": ",".join(str(x) for x in cfg.get("target_kbps", [])),
        "SVT_PRESET": str(svt.get("preset", 11)),
        "SVT_HQ_PRESET": str(svt.get("hq_preset", 4)),
        "SVT_RD_MODE": str(svt.get("rd_mode", "calibrated")),
        "SVT_KEYINT": str(svt.get("keyint", 60)),
        "SVT_LP": str(svt.get("lp", 4)),
        "SVT_RTC": "1" if svt.get("rtc") else "0",
        "SVT_SUPERRES_ENABLED": "1" if superres.get("enabled") else "0",
        "SVT_SUPERRES_MODE": str(superres.get("mode", 4)),
        "SVT_SUPERRES_DENOM": str(superres.get("denom", 9)),
        "SVT_SUPERRES_KF_DENOM": str(superres.get("kf_denom", superres.get("denom", 9))),
        "SVT_SUPERRES_QTHRES": str(superres.get("qthres", 48)),
        "SVT_SUPERRES_KF_QTHRES": str(superres.get("kf_qthres", superres.get("qthres", 48))),
        "ENC_SCALE_DENOM": str(run.get("enc_scale_denom", 2)),
        "UPSCALE_ALGOS": ",".join(run.get("upscale_algos", [])),
        "RUN_CODECS": ",".join(run.get("codecs", [])),
        "RKVC_POLICIES": ",".join(run.get("rkvc_policies", [])),
        "MLVC_QP": str(cfg.get("mlvc", {}).get("qp", 21)),
        "BENCH_CSV_MODE": str(run.get("csv_mode", "session")),
        "BENCH_PARALLEL": "1" if run.get("parallel") else "0",
        "RAMDISK_DIR": str(cfg.get("ramdisk_dir", "/dev/shm/rkvc-bench")),
        "RKVC_BUILD": paths.get("rkvc_build", ""),
        "RKVC_SR_MODEL": paths.get("rkvc_sr_model", ""),
        "FFMPEG": paths.get("ffmpeg", ""),
        "FFPROBE": paths.get("ffprobe", ""),
        "PREP_FFMPEG": paths.get("ffmpeg", ""),
        "SVT_ENC": paths.get("svt_enc", ""),
        "SVT_SUPERRES_FFMPEG": paths.get("superres_decode_ffmpeg") or "",
        "RKMPP_RC_MODE": str(cfg.get("rkmpp", {}).get("rc_mode", 2)),
        "RKMPP_GOP": str(cfg.get("rkmpp", {}).get("gop", 60)),
        "PREP_DOWNSCALE_METHOD": str(cfg.get("prep", {}).get("downscale_method", "rga-bilinear-nv12")),
        "PREP_DOWNSCALE_ALGO": str(cfg.get("prep", {}).get("downscale_algo", "bilinear")),
        "PREP_ELEM_ENCODER": str(elementary.get("encoder", "h264_rkmpp")),
        "PREP_ELEM_RC_MODE": str(elementary.get("rc_mode", 2)),
        "PREP_ELEM_QP": str(elementary.get("qp", 10)),
        "PREP_ELEM_GOP": str(elementary.get("gop", 60)),
        "MPP_LIB": paths.get("mpp_lib", ""),
        "FFMPEG_SRC": paths.get("ffmpeg_src", ""),
        "SVT_PREFIX": paths.get("svt_prefix", ""),
    }


def cmd_defaults(config_path: Path, project_root: Path) -> int:
    cfg = load_config(config_path, project_root)
    validate_config(cfg)
    for key, value in defaults_env(cfg).items():
        print(f"{key}={value}")
    return 0


def cmd_lookup(config_path: Path, project_root: Path, table: str, kbps: str) -> int:
    cfg = load_config(config_path, project_root)
    print(lookup_calibration(cfg, table, int(kbps)))
    return 0


def cmd_validate(config_path: Path, project_root: Path) -> int:
    cfg = load_config(config_path, project_root)
    validate_config(cfg)
    print(f"[ok] config {config_path}")
    return 0


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print(
            "usage: config.py {defaults|lookup|validate} CONFIG [PROJECT_ROOT] [TABLE KBPS]",
            file=sys.stderr,
        )
        return 2

    cmd = argv[1]
    if len(argv) < 3:
        print("missing config path", file=sys.stderr)
        return 2

    config_path = Path(argv[2]).resolve()
    project_root = Path(argv[3]).resolve() if len(argv) > 3 else config_path.parent.parent.resolve()

    if cmd == "defaults":
        return cmd_defaults(config_path, project_root)
    if cmd == "validate":
        return cmd_validate(config_path, project_root)
    if cmd == "lookup":
        if len(argv) < 6:
            print("usage: config.py lookup CONFIG PROJECT_ROOT TABLE KBPS", file=sys.stderr)
            return 2
        return cmd_lookup(config_path, project_root, argv[4], argv[5])

    print(f"unknown command: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
