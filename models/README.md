# 模型目录

神经视频编解码模型按变体保存为相互独立的 bundle：

```text
models/
├── mlvc/
│   ├── MLVCEncoder_<soc>.rknn
│   ├── MLVCDecoder_<soc>.rknn
│   ├── gaussian.bin
│   ├── bitest.bin
│   ├── qp_patches/
│   └── mlvc_rknn_export_manifest.json
├── mlvc-s/
│   └── （结构与 mlvc/ 相同）
└── rkvc-sr/
    ├── phase_rlfn_sr_x3.onnx
    ├── phase_rlfn_sr_x3.rknn
    ├── phase_rlfn_sr_x3.crypt.rknn  # 可选 --encrypt
    ├── sr_export_manifest.json
    ├── LICENSE.rknn-super-resolution-MIT
    └── SOURCE.md
```

RKNN、PMF、QP 补丁和 manifest 必须来自同一次、同一变体的导出，不能在
`mlvc/` 与 `mlvc-s/` 之间交叉使用。生成命令与权重要求见
[MLVC RKNN 导出](../docs/mlvc-rknn-export.md)。

`rkvc-sr/` 来自开源 Phase-RLFN 的单输入 fallback core，与旧 3 通道 RGB 模型不
兼容。ONNX、RKNN、manifest 与许可证的完整生成命令见
[Phase-RLFN 超分模型](../docs/sr-model-yuv-spec.md)。模型二进制默认被 Git 忽略。
