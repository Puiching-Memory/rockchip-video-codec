# rkvc @VERSION@ 可移植包（linux-aarch64）

一次交叉构建的 CLI + codec 插件 + 随包第三方运行库，解压即用、整包可搬迁。
构建日期 @DATE@，构建入口见仓库 `tools/portable/build.sh`。

## 目录

```text
bin/rkvc                   CLI（C++ 运行时已静态链接）
lib/rkvc/backends/*.so     codec 插件（h264h265 / av1 / mlvc / sr，按构建开关）
lib/*.so*                  随包运行库（rockchip_mpp / rknnrt / SvtAv1Enc，按需）
models/*.rkmodel           可选 RKMDL1 模型（构建时 --models 收录）
licenses/                  许可证文本与来源清单（PROVENANCE.txt）
test.sh                    包内自测
MANIFEST.sha256            全包校验和
```

## 快速开始

```bash
./test.sh                                   # 先跑自测
./bin/rkvc caps                             # 设备探测
./bin/rkvc inspect backends                 # 插件握手一览
head -c $((640*368*3/2*30)) /dev/urandom > in.nv12
./bin/rkvc encode --codec h264 --input in.nv12 --width 640 --height 368 \
    --pixfmt nv12 --output out.h264 --qp 26 --gop 30 --fps 30
```

插件与运行库按包内相对路径自动发现（`bin/../lib/rkvc/backends`），无需
`--backend-dir`；要指向别处仍可显式传 `--backend-dir DIR`。MLVC 与超分
模型用 `--model-dir models`（或逐个 `--model FILE`），`--model-id` 按导出
stem 选择。

把本包拼进下游宿主（如 `semantic-codec-sdk`）时的摆法、工具链指纹约束与
验收步骤，见仓库 `docs/portable-package.md`；注意插件不能单独搬运——它靠
`$ORIGIN/../..` 解析本包 `lib/` 里的第三方库。

## 运行环境

- aarch64 Linux，glibc ≥ 2.34（构建自 Ubuntu 22.04 交叉工具链）。
- 硬件路径：H264/HEVC 需要 `/dev/mpp_service`；MLVC/超分需要 NPU 设备节点
  与匹配的 NPU 驱动（启用时 `librknnrt.so` 随包提供）。
- `librknnrt.so` 自身动态依赖目标机的 `libstdc++.so.6` / `libgcc_s.so.1`
  （系统自带即可）；rkvc 自身产物不依赖动态 C++ 运行时。

## 自测

`./test.sh` 覆盖布局、校验和、依赖解析、插件握手、av1 软编码与 MPP 硬编
解码冒烟；无硬件项自动跳过。在 x86 上可用
`RKVC_RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu" ./test.sh`
做模拟运行（硬件项会跳过）。
