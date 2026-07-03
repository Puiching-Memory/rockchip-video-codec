# rkvc 项目交付文档

> **版本**: 0.2.1 · **硬件**: RK3588 / RK3588S · **架构**: Session + Pipeline + Codec Router (v2)

面向客户与集成方的**交付清单**。技术细节以子文档为准，避免与本页重复维护。

---

## 文档导航

| 主题 | 文档 |
|------|------|
| 快速构建与首次运行 | [getting-started.md](getting-started.md) |
| 架构与节点 | [architecture.md](architecture.md) |
| API 参考 | [api.md](api.md)（含 Doxygen 生成说明） |
| 打包与可移植包 | [packaging.md](packaging.md) |
| 测试与质量门禁 | [testing.md](testing.md) |
| 性能与 RD 基准 | [benchmark.md](benchmark.md) |
| v1 → v2 迁移 | [migration.md](migration.md) |
| YUV-native 超分（设计稿） | [sr-model-yuv-spec.md](sr-model-yuv-spec.md) |
| 发布包用户文档 | [release/README.md](release/README.md) |

---

## 交付物检查清单

### 源码与版本

- [ ] `CMakeLists.txt` `project(VERSION)` 与 `rkvc_version()` 均为 **0.2.1**
- [ ] `git submodule` 已初始化（`third_party/SVT-AV1`、`mpp`、`ffmpeg-rockchip`）
- [ ] `CHANGELOG.md` 已记录本次变更

### 构建产物

- [ ] Release 构建成功：`cmake --preset default && cmake --build --preset default`
- [ ] CLI：`rkvc_encode`、`rkvc_decode`、`rkvc_transcode`、`rkvc_info`、`rkvc_bench`、`rkvc_session_upscale`、`rkvc_yuv_upscale`
- [ ] 示例程序（含 `example_encode_file`、`example_decode_formats` 等）

### 可移植包

```bash
./scripts/package-portable.sh
./scripts/test-portable.sh build/portable/rkvc-0.2.1-linux-aarch64-portable
```

- [ ] 产物：`rkvc-0.2.1-linux-aarch64-portable.tar.gz`（约 4.5 MB）
- [ ] 包内 `./test.sh`：**99 项**全过
- [ ] 包内 `./network-e2e-test.sh` 冒烟通过（v2 占位，非完整 UDP/RTP 回环）

### 测试门禁

```bash
./scripts/test-strict.sh
export RKVC_RUN_HARDWARE_TESTS=1
ctest --test-dir build-tests -j1 -R 'test_session_' --output-on-failure
```

- [ ] `tests` preset：**16** 个 CTest 目标（9 单元 + 7 硬件子用例）
- [ ] `full-tests` preset：**18** 个 CTest 目标（+ `test_cli_args`、`test_bench_permission_failure`）
- [ ] RK3588 实机硬件用例通过（夹具自生成，无需 `tests/fixtures/`）

### 设备与环境

- [ ] SoC：RK3588 / RK3588S，BSP 内核 5.10 或 6.1
- [ ] 设备权限：`/dev/mpp_service`、`/dev/dma_heap/*`、`/dev/rga`、`/dev/dri/*`（见 [getting-started.md](getting-started.md)）
- [ ] 依赖脚本已执行：`build-svt.sh`、`rebuild-ffmpeg-rkmpp.sh`；CI 另需 `install-librga.sh`

---

## 功能验收要点（0.2.1）

| 能力 | 验收方式 |
|------|----------|
| 三策略转码 | `rkvc_transcode -p realtime\|balanced\|quality` |
| E2E fps | `rkvc_bench -i clip.mp4`（须 `-i` 指定输入） |
| RGA 后处理上采样 | `rkvc_session_upscale --enc-scale-denom 2 --post-upscale bilinear` |
| RKNN 超分（可选） | `rkvc_session_upscale --post-upscale rkvc_sr --rkvc-sr-model PATH`（需 `RKVC_ENABLE_RKNN`） |
| 多像素格式解码 | `./example_decode_formats [input.mp4]` |
| RD 基准 | `./scripts/run-bench.sh /path/to/1080p.mp4` |

---

## 已知限制

- 仅支持 RK3588 / RK3588S；可移植包须在 **aarch64 目标机**构建
- `LIVE_CAPTURE` / V4L2 与完整 UDP/RTP 回环尚未接入
- `rkvc_encode -i` 仅接受原始 NV12；`rkvc_sr` 仅 `rkvc_session_upscale` / Session API，**不支持** `rkvc_encode --post-upscale`
- `QUALITY` 依赖 SVT-AV1 软件编码，CPU 占用高于硬编
- 现网 `rkvc_sr` 模型为 RGB 域；YUV-native 规格见 [sr-model-yuv-spec.md](sr-model-yuv-spec.md)

---

## 故障排查（速查）

| 症状 | 处理 |
|------|------|
| `RKVC_ERR_PERMISSION` | 检查设备节点权限（见上） |
| `RKVC_ERR_FORMAT` | 编码用 NV12；压缩文件用 decode/transcode |
| `RKVC_ERR_HW` / `NOT_FOUND` | `rkvc_info -j`；重跑 `rebuild-ffmpeg-rkmpp.sh`、`build-svt.sh` |
| AV1 失败 | 确认 `libSvtAv1Enc.so` 存在 |
| `rkvc_bench` 无输入 | `rkvc_bench -i clip.mp4` 或先 `example_encode_file -o test.mp4` |

```bash
export RKVC_LOG_LEVEL=debug   # 或代码中 rkvc_set_log_level(AV_LOG_DEBUG)
rkvc_info -j
./build/rkvc_bench -i test.mp4
```

---

## 许可与第三方

| 组件 | 许可 | 位置 |
|------|------|------|
| ffmpeg-rockchip | LGPL/GPL | `third_party/ffmpeg-rockchip/` |
| Rockchip MPP | Apache 2.0 | `third_party/mpp/` |
| SVT-AV1 | BSD-3 / PATENTS | `third_party/SVT-AV1/` |

---

## v1 迁移

v0.1.x API 已在 0.2.0 移除。详见 [migration.md](migration.md)。
