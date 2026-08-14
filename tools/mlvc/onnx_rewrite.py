# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""为 RKNN 准备 MLVC ONNX：折叠 ``q_index_shifted``，并把 CPU fallback 算子换成 NPU 友好等价图。

对齐 ``docs/mlvc-npu-profile.md`` §7.2：

- SpaceToDepth → Reshape + Transpose + Reshape
- Max(x, const) / Min(x, const) → Clip
- Div(x, const) → Mul(x, 1/const)

``lib/node_mlvc.c`` 不向 NPU 喂 qp；导出时把 ``q_index_shifted`` 折成常量，
才能得到编码器 2 输入 / 解码器 4 输入的现网 I/O。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence, Union

PathLike = Union[str, Path]

Q_INDEX_NAMES = ("q_index_shifted", "q_index")

ENC_INPUT_KEYS = ("x", "ref_feature")
ENC_OUTPUT_KEYS = ("feature", "z_raw", "y_raw_0", "y_raw_1")
DEC_INPUT_KEYS = ("z_raw", "y_raw_0", "y_raw_1", "ref_feature")
DEC_OUTPUT_KEYS = ("x_hat", "feature")


class OnnxRewriteError(RuntimeError):
    """ONNX 图编辑失败。"""


def require_onnx():
    try:
        import onnx  # type: ignore
    except ImportError as exc:
        raise OnnxRewriteError(
            "需要 Python 包 onnx（pip install onnx）。图重写 / 折叠 q_index / inspect 都依赖它。"
        ) from exc
    return onnx


@dataclass
class TensorSpec:
    name: str
    shape: list[int | None]
    dtype: str


@dataclass
class OnnxInfo:
    path: str
    ir_version: int
    opset: int
    inputs: list[TensorSpec]
    outputs: list[TensorSpec]
    initializers: list[str]


@dataclass
class RewriteReport:
    folded_inputs: list[str] = field(default_factory=list)
    space_to_depth: int = 0
    max_to_clip: int = 0
    min_to_clip: int = 0
    div_to_mul: int = 0
    skipped: list[str] = field(default_factory=list)

    @property
    def total(self) -> int:
        return (
            len(self.folded_inputs)
            + self.space_to_depth
            + self.max_to_clip
            + self.min_to_clip
            + self.div_to_mul
        )


def _elem_type_name(onnx: Any, elem_type: int) -> str:
    mapping = {
        onnx.TensorProto.FLOAT: "float32",
        onnx.TensorProto.FLOAT16: "float16",
        onnx.TensorProto.DOUBLE: "float64",
        onnx.TensorProto.INT32: "int32",
        onnx.TensorProto.INT64: "int64",
        onnx.TensorProto.BOOL: "bool",
        onnx.TensorProto.UINT8: "uint8",
        onnx.TensorProto.INT8: "int8",
    }
    return mapping.get(elem_type, f"type{elem_type}")


def _dims_of(vi: Any) -> list[int | None]:
    dims: list[int | None] = []
    for dim in vi.type.tensor_type.shape.dim:
        if dim.HasField("dim_value"):
            dims.append(int(dim.dim_value))
        else:
            dims.append(None)
    return dims


def inspect_onnx(path: PathLike) -> OnnxInfo:
    onnx = require_onnx()
    model = onnx.load(str(path))
    opset = 0
    if model.opset_import:
        opset = int(model.opset_import[0].version)
    inits = {init.name for init in model.graph.initializer}
    inputs = [
        TensorSpec(i.name, _dims_of(i), _elem_type_name(onnx, i.type.tensor_type.elem_type))
        for i in model.graph.input
        if i.name not in inits
    ]
    outputs = [
        TensorSpec(o.name, _dims_of(o), _elem_type_name(onnx, o.type.tensor_type.elem_type))
        for o in model.graph.output
    ]
    return OnnxInfo(
        path=str(Path(path)),
        ir_version=int(model.ir_version),
        opset=opset,
        inputs=inputs,
        outputs=outputs,
        initializers=sorted(inits),
    )


def find_name(names: Iterable[str], key: str) -> str | None:
    key_l = key.lower()
    for name in names:
        if key_l in name.lower():
            return name
    return None


def classify_part(info: OnnxInfo) -> str:
    out_names = [o.name for o in info.outputs]
    in_names = [i.name for i in info.inputs]
    if find_name(out_names, "x_hat"):
        return "decoder"
    if find_name(out_names, "z_raw") and find_name(in_names, "ref_feature"):
        return "encoder"
    if find_name(out_names, "feature") and find_name(in_names, "x"):
        return "encoder"
    return "unknown"


def validate_runtime_io(info: OnnxInfo, *, part: str | None = None) -> list[str]:
    """检查折叠 q_index 之后是否仍满足 node_mlvc.c 的 I/O 约定。"""
    part = part or classify_part(info)
    in_names = [i.name for i in info.inputs]
    out_names = [o.name for o in info.outputs]
    problems: list[str] = []
    if find_name(in_names, "q_index"):
        problems.append(
            "仍有 q_index 输入；node_mlvc.c 不会写入该 tensor，请使用 --qp 折叠"
        )
    keys_in = ENC_INPUT_KEYS if part == "encoder" else DEC_INPUT_KEYS
    keys_out = ENC_OUTPUT_KEYS if part == "encoder" else DEC_OUTPUT_KEYS
    if part in ("encoder", "decoder"):
        for key in keys_in:
            if find_name(in_names, key) is None and not (
                key == "x" and find_name(in_names, "New_input_x")
            ):
                problems.append(f"{part} 缺少输入 '{key}'（现有: {in_names}）")
        for key in keys_out:
            if find_name(out_names, key) is None:
                problems.append(f"{part} 缺少输出 '{key}'（现有: {out_names}）")
        expected_n = 2 if part == "encoder" else 4
        if len(in_names) != expected_n:
            problems.append(
                f"{part} 输入数为 {len(in_names)}，运行时期望 {expected_n}（现有: {in_names}）"
            )
        if part == "encoder":
            y0 = find_name(out_names, "y_raw_0")
            y1 = find_name(out_names, "y_raw_1")
            if y0 and y1:
                i0, i1 = out_names.index(y0), out_names.index(y1)
                if i1 != i0 + 1:
                    problems.append(
                        f"编码器 y_raw_0/y_raw_1 必须相邻（C 侧用 y0 下标+1）；当前 {[y0, y1]} @ {i0},{i1}"
                    )
        if part == "decoder" and out_names and find_name(out_names[:1], "x_hat") is None:
            problems.append(f"解码器输出 0 应为 x_hat（现有: {out_names}）")
    return problems


def _value_infos(model: Any) -> list[Any]:
    graph = model.graph
    return list(graph.input) + list(graph.value_info) + list(graph.output)


def _shape_of(model: Any, name: str) -> list[int] | None:
    for vi in _value_infos(model):
        if vi.name != name:
            continue
        dims = _dims_of(vi)
        if any(d is None or d <= 0 for d in dims):
            return None
        return [int(d) for d in dims]  # type: ignore[misc]
    return None


def _elem_type_of(model: Any, name: str, default: int) -> int:
    for vi in _value_infos(model):
        if vi.name == name and vi.type.tensor_type.elem_type:
            return int(vi.type.tensor_type.elem_type)
    return default


def _init_map(model: Any) -> dict[str, Any]:
    return {init.name: init for init in model.graph.initializer}


def _constant_scalars(model: Any) -> dict[str, float]:
    """name → 标量（initializer 或 Constant 节点）。"""
    onnx = require_onnx()
    from onnx import numpy_helper  # type: ignore

    out: dict[str, float] = {}
    for init in model.graph.initializer:
        arr = numpy_helper.to_array(init)
        if arr.size == 1:
            out[init.name] = float(arr.reshape(-1)[0])
    for node in model.graph.node:
        if node.op_type != "Constant" or not node.output:
            continue
        for attr in node.attribute:
            if attr.name != "value":
                continue
            arr = numpy_helper.to_array(attr.t)
            if arr.size == 1:
                out[node.output[0]] = float(arr.reshape(-1)[0])
    return out


def _constant_array(model: Any, name: str):
    from onnx import numpy_helper  # type: ignore

    for init in model.graph.initializer:
        if init.name == name:
            return numpy_helper.to_array(init)
    for node in model.graph.node:
        if node.op_type != "Constant" or not node.output or node.output[0] != name:
            continue
        for attr in node.attribute:
            if attr.name == "value":
                return numpy_helper.to_array(attr.t)
    return None


def _unique(existing: set[str], prefix: str) -> str:
    name = prefix
    n = 0
    while name in existing:
        n += 1
        name = f"{prefix}_{n}"
    existing.add(name)
    return name


def _all_names(model: Any) -> set[str]:
    names: set[str] = set()
    for init in model.graph.initializer:
        names.add(init.name)
    for vi in _value_infos(model):
        names.add(vi.name)
    for node in model.graph.node:
        names.update(node.input)
        names.update(node.output)
        names.add(node.name)
    names.discard("")
    return names


def _make_i64_tensor(onnx: Any, name: str, values: Sequence[int]) -> Any:
    return onnx.helper.make_tensor(
        name=name,
        data_type=onnx.TensorProto.INT64,
        dims=[len(values)],
        vals=[int(v) for v in values],
    )


def fold_q_index(model: Any, qp: int, *, names: Sequence[str] = Q_INDEX_NAMES) -> list[str]:
    """把 q_index 图输入替换为 INT 常量 initializer。"""
    onnx = require_onnx()
    graph = model.graph
    inits = {init.name for init in graph.initializer}
    folded: list[str] = []
    keep_inputs = []
    for inp in list(graph.input):
        if inp.name in inits:
            keep_inputs.append(inp)
            continue
        hit = any(key.lower() in inp.name.lower() for key in names)
        if not hit:
            keep_inputs.append(inp)
            continue
        elem = inp.type.tensor_type.elem_type or onnx.TensorProto.INT32
        dims = [d for d in _dims_of(inp) if d is not None]
        if not dims:
            dims = [1]
        n_elem = 1
        for d in dims:
            n_elem *= int(d)
        tensor = onnx.helper.make_tensor(
            name=inp.name,
            data_type=elem,
            dims=dims,
            vals=[int(qp)] * n_elem,
        )
        # 若已有同名 initializer 则覆盖
        remaining = [init for init in graph.initializer if init.name != inp.name]
        del graph.initializer[:]
        graph.initializer.extend(remaining)
        graph.initializer.append(tensor)
        folded.append(inp.name)
    del graph.input[:]
    graph.input.extend(keep_inputs)
    return folded


def _replace_space_to_depth(onnx: Any, model: Any, node: Any, names: set[str], report: RewriteReport) -> list[Any] | None:
    if len(node.input) < 1 or len(node.output) < 1:
        report.skipped.append(f"SpaceToDepth {node.name}: 缺少 I/O")
        return None
    blocksize = 0
    for attr in node.attribute:
        if attr.name == "blocksize":
            blocksize = int(attr.i)
    if blocksize < 2:
        report.skipped.append(f"SpaceToDepth {node.name}: 无效 blocksize={blocksize}")
        return None
    src = node.input[0]
    dst = node.output[0]
    shape = _shape_of(model, src)
    if shape is None or len(shape) != 4:
        report.skipped.append(f"SpaceToDepth {node.name}: 无法取得固定 NCHW 形状 {src}={shape}")
        return None
    n, c, h, w = shape
    if h % blocksize or w % blocksize:
        report.skipped.append(
            f"SpaceToDepth {node.name}: {h}x{w} 不能被 blocksize={blocksize} 整除"
        )
        return None
    shape1 = [n, c, h // blocksize, blocksize, w // blocksize, blocksize]
    shape2 = [n, c * blocksize * blocksize, h // blocksize, w // blocksize]
    s1_name = _unique(names, f"{dst}__std_shape1")
    s2_name = _unique(names, f"{dst}__std_shape2")
    t1 = _unique(names, f"{dst}__std_r1")
    t2 = _unique(names, f"{dst}__std_t")
    model.graph.initializer.append(_make_i64_tensor(onnx, s1_name, shape1))
    model.graph.initializer.append(_make_i64_tensor(onnx, s2_name, shape2))
    r1 = onnx.helper.make_node("Reshape", [src, s1_name], [t1], name=_unique(names, f"{node.name}__r1"))
    tr = onnx.helper.make_node(
        "Transpose",
        [t1],
        [t2],
        perm=[0, 3, 5, 1, 2, 4],
        name=_unique(names, f"{node.name}__tr"),
    )
    r2 = onnx.helper.make_node("Reshape", [t2, s2_name], [dst], name=_unique(names, f"{node.name}__r2"))
    report.space_to_depth += 1
    return [r1, tr, r2]


def _make_scalar_tensor(onnx: Any, name: str, value: float, elem_type: int) -> Any:
    from onnx import numpy_helper  # type: ignore
    import numpy as np

    np_map = {
        onnx.TensorProto.FLOAT: np.float32,
        onnx.TensorProto.FLOAT16: np.float16,
        onnx.TensorProto.DOUBLE: np.float64,
    }
    dtype = np_map.get(elem_type, np.float32)
    return numpy_helper.from_array(np.array(value, dtype=dtype), name=name)


def _clip_nodes(
    onnx: Any,
    model: Any,
    data_name: str,
    const_value: float,
    dst: str,
    *,
    as_min: bool,
    names: set[str],
    node_name: str,
) -> list[Any]:
    bound_name = _unique(names, f"{dst}__clip_bound")
    other_name = _unique(names, f"{dst}__clip_other")
    elem = _elem_type_of(model, data_name, onnx.TensorProto.FLOAT)
    model.graph.initializer.append(_make_scalar_tensor(onnx, bound_name, const_value, elem))
    if as_min:
        model.graph.initializer.append(_make_scalar_tensor(onnx, other_name, 3.402823466e38, elem))
        inputs = [data_name, bound_name, other_name]
    else:
        model.graph.initializer.append(_make_scalar_tensor(onnx, other_name, -3.402823466e38, elem))
        inputs = [data_name, other_name, bound_name]
    return [
        onnx.helper.make_node(
            "Clip",
            inputs,
            [dst],
            name=_unique(names, f"{node_name}__clip"),
        )
    ]


def _replace_minmax(
    onnx: Any,
    model: Any,
    node: Any,
    scalars: dict[str, float],
    names: set[str],
    report: RewriteReport,
    *,
    as_min: bool,
) -> list[Any] | None:
    if len(node.input) != 2 or len(node.output) < 1:
        report.skipped.append(f"{node.op_type} {node.name}: 非二元")
        return None
    a, b = node.input[0], node.input[1]
    dst = node.output[0]
    if a in scalars and b not in scalars:
        data, const_v = b, scalars[a]
    elif b in scalars and a not in scalars:
        data, const_v = a, scalars[b]
    else:
        report.skipped.append(f"{node.op_type} {node.name}: 两侧都非常量或都是常量")
        return None
    nodes = _clip_nodes(
        onnx, model, data, const_v, dst, as_min=as_min, names=names, node_name=node.name or node.op_type
    )
    if as_min:
        report.max_to_clip += 1
    else:
        report.min_to_clip += 1
    return nodes


def _replace_div(
    onnx: Any,
    model: Any,
    node: Any,
    names: set[str],
    report: RewriteReport,
) -> list[Any] | None:
    if len(node.input) != 2 or len(node.output) < 1:
        report.skipped.append(f"Div {node.name}: 非二元")
        return None
    lhs, rhs = node.input[0], node.input[1]
    arr = _constant_array(model, rhs)
    if arr is None:
        report.skipped.append(f"Div {node.name}: 除数不是常量")
        return None
    import numpy as np  # onnx 依赖 numpy

    if (arr == 0).any():
        report.skipped.append(f"Div {node.name}: 除数含 0")
        return None
    rec = (1.0 / arr.astype("float32")).astype(arr.dtype)
    rec_name = _unique(names, f"{node.output[0]}__rcp")
    from onnx import numpy_helper  # type: ignore

    tensor = numpy_helper.from_array(rec, name=rec_name)
    model.graph.initializer.append(tensor)
    report.div_to_mul += 1
    return [
        onnx.helper.make_node(
            "Mul",
            [lhs, rec_name],
            [node.output[0]],
            name=_unique(names, f"{node.name or 'Div'}__mul"),
        )
    ]


def _reorder_value_infos(items: Sequence[Any], keys: Sequence[str]) -> list[Any]:
    remaining = list(items)
    ordered: list[Any] = []
    for key in keys:
        key_l = key.lower()
        hit = None
        for vi in remaining:
            name_l = vi.name.lower()
            if key == "x":
                if name_l == "x" or "new_input_x" in name_l:
                    hit = vi
                    break
            elif key_l in name_l:
                hit = vi
                break
        if hit is None:
            continue
        remaining.remove(hit)
        ordered.append(hit)
    return ordered + remaining


def reorder_runtime_io(model: Any, *, part: str | None = None) -> None:
    """把图 I/O 排成 ``node_mlvc.c`` 的下标约定。

    编码器输入 ``[image, ref_feature]``；输出里 ``y_raw_0`` 必须紧挨 ``y_raw_1``
    （C 侧用 ``enc_y0_out + 1`` 取 y1）。解码器输入 ``[z, y0, y1, ref]``，
    输出 ``[x_hat, feature]``。
    """
    inits = {init.name for init in model.graph.initializer}
    graph_inputs = [i for i in model.graph.input if i.name not in inits]
    info = OnnxInfo(
        path="",
        ir_version=0,
        opset=0,
        inputs=[TensorSpec(i.name, [], "") for i in graph_inputs],
        outputs=[TensorSpec(o.name, [], "") for o in model.graph.output],
        initializers=sorted(inits),
    )
    part = part or classify_part(info)
    if part == "encoder":
        new_in = _reorder_value_infos(graph_inputs, ENC_INPUT_KEYS)
        new_out = _reorder_value_infos(model.graph.output, ENC_OUTPUT_KEYS)
    elif part == "decoder":
        new_in = _reorder_value_infos(graph_inputs, DEC_INPUT_KEYS)
        new_out = _reorder_value_infos(model.graph.output, DEC_OUTPUT_KEYS)
    else:
        return
    keep_init_inputs = [i for i in model.graph.input if i.name in inits]
    del model.graph.input[:]
    model.graph.input.extend(new_in + keep_init_inputs)
    del model.graph.output[:]
    model.graph.output.extend(new_out)


def rewrite_npu_ops(model: Any) -> RewriteReport:
    """就地替换 SpaceToDepth / Max / Min / Div。"""
    onnx = require_onnx()
    try:
        inferred = onnx.shape_inference.infer_shapes(model)
        model.CopyFrom(inferred)
    except Exception:
        pass
    report = RewriteReport()
    names = _all_names(model)
    scalars = _constant_scalars(model)
    new_nodes: list[Any] = []
    for node in list(model.graph.node):
        replacement: list[Any] | None = None
        if node.op_type == "SpaceToDepth":
            replacement = _replace_space_to_depth(onnx, model, node, names, report)
        elif node.op_type == "Max":
            replacement = _replace_minmax(
                onnx, model, node, scalars, names, report, as_min=True
            )
        elif node.op_type == "Min":
            replacement = _replace_minmax(
                onnx, model, node, scalars, names, report, as_min=False
            )
        elif node.op_type == "Div":
            replacement = _replace_div(onnx, model, node, names, report)
        if replacement is None:
            new_nodes.append(node)
        else:
            new_nodes.extend(replacement)
    del model.graph.node[:]
    model.graph.node.extend(new_nodes)
    return report


def prepare_onnx(
    src: PathLike,
    dst: PathLike,
    *,
    qp: int | None = 21,
    rewrite: bool = True,
    fold: bool = True,
) -> tuple[OnnxInfo, RewriteReport]:
    onnx = require_onnx()
    model = onnx.load(str(src))
    report = RewriteReport()
    if fold and qp is not None:
        report.folded_inputs = fold_q_index(model, int(qp))
    if rewrite:
        extra = rewrite_npu_ops(model)
        report.space_to_depth = extra.space_to_depth
        report.max_to_clip = extra.max_to_clip
        report.min_to_clip = extra.min_to_clip
        report.div_to_mul = extra.div_to_mul
        report.skipped.extend(extra.skipped)
    reorder_runtime_io(model)
    out = Path(dst)
    out.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(out))
    info = inspect_onnx(out)
    return info, report
