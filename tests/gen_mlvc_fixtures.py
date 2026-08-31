#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""再生 tests/fixtures/mlvc/* 跨语言契约样本（PMF1 + QPP1）。

这些文件由 Python 生产工具（tools/mlvc/pmf.py / qppatch.py）生成，
供 C 侧契约测试 tests/test_mlvc_contract.c 加载，验证 Python 与 C 对
二进制格式的理解一致。

用法:
    python3 tests/gen_mlvc_fixtures.py            # 写入 tests/fixtures/mlvc/
    python3 tests/gen_mlvc_fixtures.py --check    # 仅校验与仓库样本一致
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "mlvc"))

import pmf  # noqa: E402
import qppatch  # noqa: E402

# 与 tests/test_mlvc_export.py 的 GAUSSIAN_JSON / BITEST_JSON 保持一致
GAUSSIAN_JSON = {
    "pmf_lengths": [2, 3],
    "pmf_offsets": [0, 2],
    "pmf_table": [1, 2, 3, 4, 5],
    "scale_min": 0.11,
    "scale_max": 16.0,
    "scale_levels": 128,
    "index_space": True,
}

BITEST_JSON = {
    "pmf_lengths": [4, 5, 6, 7],
    "pmf_offsets": [0, 4, 9, 15],
    "pmf_table": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22],
    "qp_num": 2,
    "channels": 2,
}

# 模拟 rknn 模型缓冲区：QPP1 是对基座 memcpy 差异区间，用任意二进制即可
QP_BASE = bytes(range(256)) * 16  # 4096 B


def build() -> dict[str, bytes]:
    qp_target = bytearray(QP_BASE)
    qp_target[100] ^= 0xFF
    qp_target[3000:3010] = b"RKVC-QPP1!"
    return {
        "gaussian.bin": pmf.json_to_pmf1(GAUSSIAN_JSON, kind="gaussian"),
        "bitest.bin": pmf.json_to_pmf1(BITEST_JSON, kind="bitest"),
        "qppatch_base.bin": QP_BASE,
        "qppatch_target.bin": bytes(qp_target),
        "qppatch_qp1.bin": qppatch.encode(QP_BASE, bytes(qp_target), 1),
    }


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "tests" / "fixtures" / "mlvc")
    ap.add_argument("--check", action="store_true", help="仅校验现有样本一致")
    args = ap.parse_args()

    files = build()
    ok = True
    for name, data in sorted(files.items()):
        path = args.out / name
        if args.check:
            if not path.exists():
                print(f"FAIL: {name} 缺失（运行 python3 tests/gen_mlvc_fixtures.py 再生）")
                ok = False
                continue
            actual = _sha256(path.read_bytes())
            expect = _sha256(data)
            if actual != expect:
                print(f"FAIL: {name} 与 Python 工具产物不一致 (sha256 {actual[:12]} != {expect[:12]})")
                ok = False
            else:
                print(f"ok: {name} 一致")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f"写 {path} ({len(data)} B)")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
