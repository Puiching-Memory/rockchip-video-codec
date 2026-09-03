"""SBOM（CycloneDX 1.5）与许可证聚合（P5）。

- ``write_sbom``：从 sysroot 锁文件 + 适配器清单生成 CycloneDX JSON，
  作为包内 ``bom.cyclonedx.json``，须先于 seal 调用以纳入 SHA256SUMS。
- ``aggregate_legal``：归集本仓与 third_party 许可证到 ``legal/``。
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_LOCK = Path(__file__).resolve().parent / "sysroot.focal-arm64.lock.json"

# 交付包实际携带/链接的第三方组件（名称, 许可证 SPDX, 本仓路径）
_ADAPTERS = [
    ("ffmpeg-rockchip", "LGPL-2.1-or-later", "patches/ffmpeg-rockchip"),
    ("rockchip-mpp", "Apache-2.0", "third_party/mpp"),
    ("SVT-AV1", "BSD-3-Clause-Clear", "third_party/SVT-AV1"),
    ("librga", "Apache-2.0", "third_party/librga"),
    ("rknn-runtime", "LicenseRef-Rockchip-RKNN", ""),
]

# 许可证归集候选文件名（有界清单，按组件顺序）
_LEGAL_FILES = {
    "rockchip-mpp": ["LICENSES/Apache-2.0", "LICENSE"],
    "SVT-AV1": ["LICENSE.md", "LICENSE"],
    "librga": ["COPYING", "LICENSE"],
}


def write_sbom(pkg_root: Path, version: str, target: str) -> Path:
    """生成 CycloneDX 1.5 SBOM 到包根；返回文件路径。"""
    components = []
    if _LOCK.exists():
        lock = json.loads(_LOCK.read_text())
        for pkg in lock.get("packages", []):
            components.append({
                "type": "library",
                "name": pkg["name"],
                "version": pkg["version"],
                "purl": f"pkg:deb/ubuntu/{pkg['name']}@{pkg['version']}?arch=arm64&distro=focal",
                "scope": "required",
                "description": "sysroot 组件（SHA-256 锁定）",
            })
    for name, license_id, rel in _ADAPTERS:
        if name == "rknn-runtime" and not any(
                (pkg_root / "lib").glob("librknnrt.so*")):
            continue
        components.append({
            "type": "library",
            "name": name,
            "licenses": [{"license": {"id": license_id}}],
            "scope": "required",
        })

    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "library",
                "name": "rkvc",
                "version": version,
                "licenses": [{"license": {"id": "AGPL-3.0-or-later"}}],
            },
            "properties": [
                {"name": "rkvc:target", "value": target},
            ],
        },
        "components": components,
    }
    out = pkg_root / "bom.cyclonedx.json"
    out.write_text(json.dumps(sbom, indent=2, ensure_ascii=False) + "\n")
    return out


def aggregate_legal(pkg_root: Path) -> Path:
    """归集许可证文本到 ``legal/``；返回目录。缺失文件跳过（如实）。"""
    dest = pkg_root / "legal"
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(_REPO_ROOT / "LICENSE", dest / "rkvc.LICENSE")
    rknn_license = pkg_root / "share" / "licenses" / "rknn-runtime" / "LICENSE"
    if rknn_license.is_file():
        shutil.copy2(rknn_license, dest / "rknn-runtime.LICENSE")
    for name, _lic, rel in _ADAPTERS:
        src_dir = _REPO_ROOT / rel
        for cand in _LEGAL_FILES.get(name, []):
            src = src_dir / cand
            if src.is_file():
                shutil.copy2(src, dest / f"{name}.{src.name}")
                break
    return dest
