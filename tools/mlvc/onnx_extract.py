# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 梦归云帆
"""把图边界上的纯搬运算子拆出 RKNN 图，改由运行时 CPU 计算。

只动 **图头/图尾** 的 permute / clip，不切开中间残差：

- 编码器头 ``SpaceToDepth(x)`` → 新输入就是 unshuffle 后的 tensor
- 解码器尾 ``DepthToSpace → Clip → x_hat`` → 新输出是 shuffle 前的 head conv

这些是元素重排（外加 ``Clip(0,1)``），CPU 实现可以与 ONNX 位级一致。
中间的 Conv+DepthToSpace 若拆成多段 ``rknn_run``，编译器可能融成不同的
ConvTranspose，**不保证** 与整图 1:1，不在这里做。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Sequence, Union

from onnx_rewrite import OnnxRewriteError, require_onnx

PathLike = Union[str, Path]


@dataclass
class ExtractReport:
    op: str
    blocksize: int
    in_name: str
    out_name: str
    in_shape: list[int]
    out_shape: list[int]
    clip_lo: float | None = None
    clip_hi: float | None = None
    mode: str = ""
    notes: list[str] = field(default_factory=list)


def _graph_input_names(model: Any) -> set[str]:
    inits = {init.name for init in model.graph.initializer}
    return {i.name for i in model.graph.input if i.name not in inits}


def _set_dims(vi: Any, dims: Sequence[int]) -> None:
    shape = vi.type.tensor_type.shape
    del shape.dim[:]
    for d in dims:
        dim = shape.dim.add()
        dim.dim_value = int(d)


def _find_value_info(model: Any, name: str):
    for vi in list(model.graph.input) + list(model.graph.value_info) + list(model.graph.output):
        if vi.name == name:
            return vi
    return None


def _shape_of(model: Any, name: str) -> list[int] | None:
    vi = _find_value_info(model, name)
    if vi is None:
        return None
    dims = []
    for dim in vi.type.tensor_type.shape.dim:
        if not dim.HasField("dim_value") or dim.dim_value <= 0:
            return None
        dims.append(int(dim.dim_value))
    return dims


def _blocksize(node: Any) -> int:
    for attr in node.attribute:
        if attr.name == "blocksize":
            return int(attr.i)
    return 0


def _constant_scalar(model: Any, name: str) -> float | None:
    from onnx import numpy_helper  # type: ignore

    for init in model.graph.initializer:
        if init.name != name:
            continue
        arr = numpy_helper.to_array(init)
        if arr.size == 1:
            return float(arr.reshape(-1)[0])
        return None
    for node in model.graph.node:
        if node.op_type != "Constant" or not node.output or node.output[0] != name:
            continue
        for attr in node.attribute:
            if attr.name != "value":
                continue
            arr = numpy_helper.to_array(attr.t)
            if arr.size == 1:
                return float(arr.reshape(-1)[0])
            return None
    return None


def _rewire_uses(model: Any, old: str, new: str) -> int:
    n = 0
    for node in model.graph.node:
        for i, inp in enumerate(list(node.input)):
            if inp == old:
                node.input[i] = new
                n += 1
    for i, out in enumerate(list(model.graph.output)):
        if out.name == old:
            model.graph.output[i].name = new
            n += 1
    return n


def _node_key(node: Any) -> str:
    if node.name:
        return node.name
    return f"{node.op_type}:{','.join(node.output)}"


def _drop_nodes(model: Any, drop: set[str]) -> None:
    keep = [node for node in model.graph.node if _node_key(node) not in drop]
    del model.graph.node[:]
    model.graph.node.extend(keep)


def _drop_value_info(model: Any, names: set[str]) -> None:
    keep = [vi for vi in model.graph.value_info if vi.name not in names]
    del model.graph.value_info[:]
    model.graph.value_info.extend(keep)


def extract_leading_space_to_depth(model: Any) -> ExtractReport:
    """删除以图输入为源的 ``SpaceToDepth``，把该输入改成 unshuffle 后的形状。"""
    onnx = require_onnx()
    graph_ins = _graph_input_names(model)
    target = None
    for node in model.graph.node:
        if node.op_type != "SpaceToDepth" or not node.input or not node.output:
            continue
        if node.input[0] in graph_ins:
            target = node
            break
    if target is None:
        raise OnnxRewriteError("没有以图输入为源的 SpaceToDepth")
    bs = _blocksize(target)
    if bs < 2:
        raise OnnxRewriteError(f"SpaceToDepth blocksize 无效: {bs}")
    src, dst = target.input[0], target.output[0]
    in_shape = _shape_of(model, src)
    if in_shape is None or len(in_shape) != 4:
        try:
            inferred = onnx.shape_inference.infer_shapes(model)
            model.CopyFrom(inferred)
        except Exception as exc:
            raise OnnxRewriteError(f"无法推断 SpaceToDepth 输入形状: {exc}") from exc
        in_shape = _shape_of(model, src)
    if in_shape is None or len(in_shape) != 4:
        raise OnnxRewriteError(f"SpaceToDepth 输入 {src} 无固定 NCHW 形状: {in_shape}")
    n, c, h, w = in_shape
    if h % bs or w % bs:
        raise OnnxRewriteError(f"{h}x{w} 不能被 blocksize={bs} 整除")
    out_shape = [n, c * bs * bs, h // bs, w // bs]
    uses = _rewire_uses(model, dst, src)
    if uses < 1:
        raise OnnxRewriteError("SpaceToDepth 输出没有消费者")
    _drop_nodes(model, {_node_key(target)})
    _drop_value_info(model, {dst})
    vi = _find_value_info(model, src)
    if vi is None:
        raise OnnxRewriteError(f"找不到图输入 {src}")
    _set_dims(vi, out_shape)
    try:
        inferred = onnx.shape_inference.infer_shapes(model)
        model.CopyFrom(inferred)
    except Exception as exc:
        # 形状已手写，推理失败不阻断
        notes = [f"shape_inference: {exc}"]
    else:
        notes = []
    return ExtractReport(
        op="SpaceToDepth",
        blocksize=bs,
        in_name=src,
        out_name=src,
        in_shape=in_shape,
        out_shape=out_shape,
        notes=notes + [f"rewired_uses={uses}"],
    )


def extract_trailing_depth_to_space(model: Any) -> ExtractReport:
    """删除 ``x_hat`` 前的 ``DepthToSpace`` + 可选 ``Clip``，图输出改为 shuffle 前的 tensor。"""
    onnx = require_onnx()
    x_hat = None
    for vi in model.graph.output:
        if "x_hat" in vi.name.lower():
            x_hat = vi
            break
    if x_hat is None:
        raise OnnxRewriteError("没有名为 x_hat 的图输出")
    producers = {out: node for node in model.graph.node for out in node.output}
    tail = producers.get(x_hat.name)
    if tail is None:
        raise OnnxRewriteError(f"{x_hat.name} 没有生产者")
    clip_lo = clip_hi = None
    clip_node = None
    d2s = None
    if tail.op_type == "Clip":
        clip_node = tail
        if len(tail.input) >= 3:
            clip_lo = _constant_scalar(model, tail.input[1])
            clip_hi = _constant_scalar(model, tail.input[2])
        d2s = producers.get(tail.input[0])
    elif tail.op_type == "DepthToSpace":
        d2s = tail
    if d2s is None or d2s.op_type != "DepthToSpace":
        raise OnnxRewriteError(
            f"x_hat 前不是 DepthToSpace/Clip(DepthToSpace)，而是 {tail.op_type}"
        )
    bs = _blocksize(d2s)
    if bs < 2:
        raise OnnxRewriteError(f"DepthToSpace blocksize 无效: {bs}")
    mode = "DCR"
    for attr in d2s.attribute:
        if attr.name == "mode" and attr.s:
            raw = attr.s.decode() if isinstance(attr.s, (bytes, bytearray)) else str(attr.s)
            if raw:
                mode = raw
    conv_out = d2s.input[0]
    in_shape = _shape_of(model, conv_out)
    out_shape = _shape_of(model, d2s.output[0])
    if in_shape is None or out_shape is None:
        try:
            inferred = onnx.shape_inference.infer_shapes(model)
            model.CopyFrom(inferred)
        except Exception:
            pass
        in_shape = _shape_of(model, conv_out) or in_shape
        out_shape = _shape_of(model, d2s.output[0]) or out_shape
    if in_shape is None:
        raise OnnxRewriteError(f"无法取得 DepthToSpace 输入形状 {conv_out}")

    # x_hat 改挂到 head conv 输出；删 D2S / Clip
    drop = {_node_key(d2s)}
    if clip_node is not None:
        drop.add(_node_key(clip_node))
    _drop_nodes(model, drop)
    _drop_value_info(model, {d2s.output[0], x_hat.name})

    # 把图输出 x_hat 换成 conv_out 的 value info（改名以保持运行时按名绑定）
    conv_vi = _find_value_info(model, conv_out)
    new_out = onnx.helper.make_tensor_value_info(
        x_hat.name,
        x_hat.type.tensor_type.elem_type,
        in_shape,
    )
    # 若 conv_out 已在 outputs 则去掉；替换 x_hat
    new_outputs = []
    replaced = False
    for vi in model.graph.output:
        if vi.name == x_hat.name:
            new_outputs.append(new_out)
            replaced = True
        elif vi.name == conv_out:
            continue
        else:
            new_outputs.append(vi)
    if not replaced:
        new_outputs.insert(0, new_out)
    del model.graph.output[:]
    model.graph.output.extend(new_outputs)

    # 图内其它节点若仍引用 x_hat / D2S 输出则失败（尾部算子不应再被用）
    live = set()
    for node in model.graph.node:
        live.update(node.input)
        live.update(node.output)
    if d2s.output[0] in live or (clip_node and clip_node.output[0] in live and clip_node.output[0] != x_hat.name):
        raise OnnxRewriteError("DepthToSpace/Clip 输出仍被图内节点引用，不是纯图尾")

    # 让 head conv 的输出名保持 conv_out，再加 Identity 到 x_hat，避免悬空
    # 更简单：把 head conv 的输出直接改名为 x_hat
    renamed = 0
    for node in model.graph.node:
        for i, out in enumerate(list(node.output)):
            if out == conv_out:
                node.output[i] = x_hat.name
                renamed += 1
        for i, inp in enumerate(list(node.input)):
            if inp == conv_out:
                node.input[i] = x_hat.name
    if conv_vi is not None:
        conv_vi.name = x_hat.name
        _set_dims(conv_vi, in_shape)

    try:
        inferred = onnx.shape_inference.infer_shapes(model)
        model.CopyFrom(inferred)
    except Exception as exc:
        notes = [f"shape_inference: {exc}"]
    else:
        notes = []
    if renamed != 1:
        notes.append(f"renamed_outputs={renamed}")
    return ExtractReport(
        op="DepthToSpace",
        blocksize=bs,
        in_name=x_hat.name,
        out_name=x_hat.name,
        in_shape=list(in_shape),
        out_shape=list(out_shape) if out_shape else [],
        clip_lo=clip_lo,
        clip_hi=clip_hi,
        mode=mode,
        notes=notes,
    )


def extract_file(src: PathLike, dst: PathLike, kind: str) -> ExtractReport:
    onnx = require_onnx()
    model = onnx.load(str(src))
    if kind == "space_to_depth":
        report = extract_leading_space_to_depth(model)
    elif kind == "depth_to_space":
        report = extract_trailing_depth_to_space(model)
    else:
        raise OnnxRewriteError(f"未知 extract kind: {kind}")
    out = Path(dst)
    out.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(out))
    return report
