#!/usr/bin/env python3
# 冒烟测试：构造带 Gather(q_table, q_index_shifted) 的玩具 ONNX，验证 make_qp_dynamic
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np
import onnx
from onnx import TensorProto, helper

import onnx_rewrite as rw
import qptab

HW = [1, 4, 2, 2]


def build_toy(in_names, out_names, table_names, path):
    inputs = [helper.make_tensor_value_info(n, TensorProto.FLOAT16, HW) for n in in_names]
    inputs.append(helper.make_tensor_value_info("q_index_shifted", TensorProto.INT32, [1]))
    nodes = []
    inits = []
    prev = in_names[0]
    for tname in table_names:
        table = np.arange(72 * 4, dtype=np.float32).reshape(72, 4, 1, 1).astype(np.float16)
        inits.append(helper.make_tensor(tname, TensorProto.FLOAT16, table.shape, table.tobytes(), raw=True))
        g = helper.make_node("Gather", [tname, "q_index_shifted"], [f"{tname}_row"], axis=0)
        m = helper.make_node("Mul", [prev, f"{tname}_row"], [f"{tname}_mul"])
        nodes.extend([g, m])
        prev = f"{tname}_mul"
    outputs = []
    for oname in out_names:
        nodes.append(helper.make_node("Identity", [prev], [oname]))
        outputs.append(helper.make_tensor_value_info(oname, TensorProto.FLOAT16, HW))
    graph = helper.make_graph(nodes, "toy", inputs, outputs, initializer=inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.checker.check_model(model)
    onnx.save(model, str(path))


tmp = Path(".temp/smoke_qp_dynamic")
tmp.mkdir(parents=True, exist_ok=True)
src = tmp / "toy.onnx"
build_toy(
    ["src_yuv_x", "input_ref_feature"],
    ["out_feature", "out_z_raw", "out_y_raw_0", "out_y_raw_1"],
    ["model.q_encoder", "model.q_decoder"],
    src,
)

dst = tmp / "toy_dyn.onnx"
info, report = rw.prepare_onnx(src, dst, qp=None, rewrite=False, fold=False, qp_dynamic=True)
in_names = [s.name for s in info.inputs]
print("inputs:", in_names)
assert "q_index_shifted" not in in_names, "q_index 应被移除"
assert "q_encoder_row" in in_names and "q_decoder_row" in in_names, f"缺 q_*_row: {in_names}"
assert len(report.qp_tables) == 2
for t in report.qp_tables:
    print(f"table {t.name} rows={t.rows} cols={t.cols} src={t.source}")
    assert t.rows == 72 and t.cols == 4

kind = rw.classify_part(info)
print("classify:", kind)
assert kind == "encoder"
warns = rw.validate_runtime_io(info, part="encoder", qp_dynamic=True)
print("validate warns:", warns)
assert not warns

model = onnx.load(str(dst))
onnx.checker.check_model(model)
gather_left = [n for n in model.graph.node if n.op_type == "Gather"]
assert not gather_left, "Gather 应全部移除"
init_left = [i.name for i in model.graph.initializer]
assert not init_left, f"initializer 应清空: {init_left}"
for n in model.graph.node:
    if n.op_type == "Mul":
        assert n.input[1].endswith("_row"), f"Mul 输入未重定向: {n.input}"

# qptab 写入 + 回读校验（与 C 解析器格式对齐）
qbin = tmp / "qptab_encoder.bin"
n = qptab.write_qptab(report.qp_tables, qbin)
raw = qbin.read_bytes()
assert n == len(raw)
assert raw[:4] == b"QPT1"
import struct
tc = struct.unpack_from("<I", raw, 4)[0]
assert tc == 2
off = 8
for expect in ("q_encoder_row", "q_decoder_row"):
    nlen = struct.unpack_from("<I", raw, off)[0]; off += 4
    name = raw[off:off + nlen].decode(); off += nlen
    rows, cols = struct.unpack_from("<II", raw, off); off += 8
    data = raw[off:off + rows * cols * 2]; off += rows * cols * 2
    assert name == expect and rows == 72 and cols == 4 and len(data) == 576
assert off == len(raw)
print(f"qptab bytes={n} tables={tc} 格式回读 OK")

# 非 Gather 消费表 → 必须报错
bad = tmp / "bad.onnx"
build_toy(["src_yuv_x"], ["out_feature"], ["model.q_encoder"], bad)
m = onnx.load(str(bad))
m.graph.node.append(helper.make_node("Identity", ["model.q_encoder"], ["leak"]))
m.graph.output.append(helper.make_tensor_value_info("leak", TensorProto.FLOAT16, [72, 4, 1, 1]))
onnx.save(m, str(bad))
try:
    rw.prepare_onnx(bad, tmp / "bad_out.onnx", qp=None, rewrite=False, fold=False, qp_dynamic=True)
except rw.OnnxRewriteError as e:
    print("非 Gather 消费表正确报错:", e)
else:
    raise AssertionError("应报错的坏图未报错")

print("SMOKE PASS")
