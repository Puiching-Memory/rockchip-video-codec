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
└── rkvc_sr_x3.crypt.rknn
```

RKNN、PMF、QP 补丁和 manifest 必须来自同一次、同一变体的导出，不能在
`mlvc/` 与 `mlvc-s/` 之间交叉使用。生成命令与权重要求见
[MLVC RKNN 导出](../docs/mlvc-rknn-export.md)。

这些生成物默认被 Git 忽略；`rkvc_sr_x3.crypt.rknn` 是独立的超分模型，不属于
任一 MLVC bundle。
