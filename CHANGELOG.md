# Changelog

本文档记录 rkvc 各版本的主要变更。

## [Unreleased]

### 新增

- **MLVC ONNX → RKNN 导出工具**（`tools/mlvc/`）：`python export_rknn.py --from-mlvc` 浅克隆 microsoft/mlvc 并跑 `convert.py export --target-device generic`，再折叠 `q_index_shifted`、把 SpaceToDepth/Max/Div 换成 NPU 友好算子，经 rknn-toolkit2 转 FP16 `.rknn`，PMF JSON 写成 PMF1。环境：`cd tools/mlvc && uv sync`。无 toolkit 时可用 `--skip-rknn` / `--pmf-only`。文档见 `docs/mlvc-rknn-export.md`。
- **MLVC 多 QP 单模型（QPP1）**：同尺寸多 qp `.rknn` 打成区间补丁（`tools/mlvc/qppatch.py` / `make_qp_patches.py`）。`rkvc_transcode --mlvc-qp-patch-dir` 在 `rknn_init` 前对基座打一次补丁（编码用 `--mlvc-qp`，解码用容器头 qp）；缺补丁或 CRC 失败则打开失败。C 应用器 `lib/qppatch.c`，测试 `tests/test_qppatch.c`。

### 变更

- **bench：MLVC 对照图改为「论文 Semantic 粗比 + 本机实测」**：只叠 Semantic（MS-SSIM），不叠 H.264+LDPC。横轴 CBR=bpp/8，纵轴线性相似度；实测为 FourPeople 640×368 重跑。主 RD 默认仍不叠文献（`LITERATURE=1` 才启用）。

### 性能

- **MLVC 推理性能优化（RK3588，编码 +24%）**（`lib/node_mlvc.c`）：经分阶段插桩定位，单帧编码 116 ms 中 NPU `rknn_run` 占 79%（92 ms）、fp16 标量位运算转换占 16%（18.7 ms）；解码 192 ms 中 NPU `rknn_run` 占 86%（153 ms，模型固有计算，软件层无法突破）。两处可落地优化：
  - **fp16 转换硬件化**：`f32_to_f16` / `f16_to_f32` 此前为纯软件逐位运算（每元素 ~15 条指令 + 分支）。AArch64 上改用原生 `__fp16` 类型，各编译为单条 `fcvt` 指令（IEEE round-to-nearest-even，与软件实现**逐位等价**），并使编译器能对调用处循环自动向量化。其它架构保留原可移植软件实现（`#if defined(__ARM_NEON)` 守卫）。
  - **NPU 多核调度（板卡 profile 驱动）**：`rknn_init` 后按板卡 profile 的 `npu_cores` 显式设置 `rknn_set_core_mask`，启用全部 NPU 核心让运行时在核间分配算子，显著降低编码推理延迟。分层设计避免硬编码：
    - **板卡抽象层**（`lib/board.h` / `lib/board.c`）新增 `rkvc_board_profile::npu_cores` 字段，描述硬件事实（NPU 计算核心数）：RK3588=3（每核 2 TOPS，共 6 TOPS）、RV1126B=1。该层不依赖 `rknn_api.h`。
    - **MLVC/RKNN 层**（`lib/node_mlvc.c`）新增 `mlvc_npu_core_mask(int cores)`，负责 `npu_cores → RKNN core_mask` 映射（3→`CORE_0_1_2`、2→`CORE_0_1`、1→不调用保持默认）；多核（`npu_cores>1`）时启用，单核平台保持默认单核行为。
  - **实测**（640×368，受控 A/B 交替 3 轮排除 NPU 争用/热噪声）：编码 8.09 → 10.04 fps（+24%），解码 5.13 → 5.25 fps（NPU 模型固有计算为瓶颈，软件层无收益）。
  - **正确性**：端到端 `.mlvc` 码流逐字节一致（sha256 相同，含 `__fp16` 位等价 + `core_mask` 不改变计算结果双重验证）；解码往返 `.mlvc → .yuv` 输出尺寸精确、Y 平面内容有效。


## [0.3.0] - 2026-08-12

### 变更

- **多板卡架构重构（首板 RV1126B）**：项目此前完全基于 RK3588 假设（`rkvc_query_caps` 硬编码 `7680×4320`、CMake/文档/CLI 文案均写死 "RK3588"）。现引入板卡 profile 抽象层，将所有板级常量收拢到单一数据源，支持多板卡。
  - **新增板卡抽象层**：`include/rkvc/board.h`（公共：`rkvc_board_id` 枚举、`rkvc_board_id_name()` / `rkvc_board_id_from_name()` / `rkvc_detect_board()`）；`lib/board.h`（内部 `rkvc_board_profile`：最大编/解分辨率、VPU 各编解码硬件支持、NPU TOPS、RGA）；`lib/board.c`（profile 表 + 运行时探测）。
  - **板卡探测优先级**：`RKVC_BOARD` 环境变量 > `/proc/device-tree/compatible` 自动探测 > 编译期默认 `RKVC_DEFAULT_BOARD`（CMake `RKVC_BOARD`，默认 `rk3588`）。
  - **profile 表**：`RKVC_BOARD_RK3588`（权威值：7680×4320，AV1 硬解 6 TOPS NPU）；`RKVC_BOARD_RV1126B`（Rockchip 官网 RV11 系列页权威值：4K@45 编码 / 4K@30 解码，H.264/H.265，无 AV1 硬件编解码，3 TOPS NPU，Quad Cortex-A53）。
  - **`rkvc_caps` 新增 `board` 字段**（`rkvc_board_id`）；`rkvc_query_caps`（`lib/init.c`）改用 profile：`max_width/height`、`board` 取自 profile，硬件编解码能力 = 运行时探测 ∩ 板卡 VPU 硬件支持（软件 SVT-AV1 编码器不做板卡门控）。`rkvc_info` 文本/JSON 输出新增 `board` 字段。
  - **CMake**：新增 `RKVC_BOARD` 缓存选项（`rk3588|rv1126b`）映射 `RKVC_DEFAULT_BOARD`；`DESCRIPTION` / `CPACK_PACKAGE_DESCRIPTION_SUMMARY` 去 "RK3588" 化为 "Rockchip multi-SoC"。
  - **验证**：三层探测全部验证——`RKVC_BOARD=rv1126b` 环境覆盖 → 报告 rv1126b / max 1920×1088；RK3588 真机 device-tree → rk3588 / 7680×4320；device-tree 屏蔽后回退编译期默认；AV1 硬解门控生效（rk3588 enc/dec=1/1，rv1126b enc/dec=1/0）。全树编译通过（shared + static + CLI + examples）。

- **修复解码器输出布局 bug：NC1HWC2→NV12 stride 错误**（`lib/node_mlvc.c`）：解码器 `x_hat` 输出为 RKNN native NC1HWC2 fp16 格式（dims `[1, C1=1, H, W, C2=8]`），每像素占 8 个 fp16（3 有效通道 + 5 填充）。此前 `nhwc_fp16_to_nv12` 按 NHWC stride=3 读取（`nhwc[o*3+c]`），导致像素数据错位——每隔 3 个值才读到 1 个有效通道，其余为填充零，输出 Y 平面约 62.5% 像素为 0（PSNR ~8dB，完全不可用）。原为 C++ 版本的遗留 bug。
  - **修复**：新增 `nc1hwc2_fp16_to_nv12(src, W, H, c2, out)`，按实际 C2 stride 读取（`src[o*c2+c]`）；`dec_resolve_geom` 从 `native_out_attr[0].dims[4]` 读取 C2 存入 `d->OUT_C2`。
  - **修复后质量**（640×368×72f @ ~66.7 kbps / ~0.06 bpp）：PSNR Y **26.79 dB**（avg 28.39），SSIM Y **0.798**（All 0.840）——符合 MLVC 超低码率神经编解码预期。
  - 码流不变（编码侧无改动，MD5 一致），仅解码重建帧正确化。


- **bench/ 同步 MLVC `neural` 档位**（`bench/run_rd_benchmark.sh` / `bench/config.json` / `bench/tools/config.py` / `bench/plot_rd_curve.py` / `bench/README.md`）：RD 基准新增 `rkvc-neural` codec 路线，与既有 `rkvc-realtime/balanced/quality/offline` 平行纳入自动扫描。
  - **`run_rkvc_neural()`**（`bench/run_rd_benchmark.sh`）：缩放到 MLVC 固定 640×368 → `rkvc_transcode -p neural` 编码 `.mlvc` → `rkvc_transcode` 解码 `.yuv`（NV12）→ `measure_quality_nv12` 测质（PSNR/SSIM）。MLVC 不参与码率扫描（用 qp 参数化），与其他 codec 共享 CSV 行格式。
  - **模型/PMF 路径**：`MLVC_ENC_MODEL` / `MLVC_DEC_MODEL` / `MLVC_GAUSSIAN_PMF` / `MLVC_BITEST_PMF` 环境变量，默认 `models/MLVCEncoder_rk3588.rknn` / `MLVCDecoder_rk3588.rknn` / `gaussian.bin` / `bitest.bin`；`MLVC_QP`（默认 21）、`MLVC_W`/`MLVC_H`（默认 640/368）。
  - **config.json** 新增 `"mlvc"` 节；`run.rkvc_policies` 新增 `"neural"`。**config.py** 新增 `MLVC_QP` 导出。**plot_rd_curve.py** 新增 `rkvc-neural` 显示名/颜色/标记/z-order。**README.md** 新增 codec 行 + 环境变量说明。
  - **验证**：脚本语法（`bash -n`）通过；config.py 正确导出 `RKVC_POLICIES` 含 `neural` + `MLVC_QP=21`；`run_rkvc_neural` 逻辑端到端验证（编码 19 562 B → 解码 72 帧 NV12 → PSNR/SSIM 测质输出）。


- **新增 `neural` 语义档位**（`RKVC_POLICY_NEURAL`）：MLVC 现在与其他编解码器一样有一等语义档位，`-p neural` 在 `-c auto` 时自动选择 MLVC 神经编解码器，与现有档位平行：
  - `realtime` → H.264 RKMPP
  - `balanced` → HEVC RKMPP
  - `quality` → AV1 SVT preset 11
  - `offline` → AV1 SVT preset 4
  - **`neural` → MLVC（NPU + rANS）**（新增）
  - `include/rkvc/policy.h` 枚举新增 `RKVC_POLICY_NEURAL`；`lib/router.c` `rkvc_route_resolve` 自动选择新增 `neural → fill_mlvc` 分支，`rkvc_policy_name` 新增 `"neural"` 映射。
  - `cli/rkvc_transcode.c` / `cli/rkvc_encode.c` `parse_policy` 新增 `"neural"` 解析；`cli/rkvc_bench.c` bench 表新增 `NEURAL (MLVC)` 档位。
  - `tests/test_router.c` 新增 `test_neural_routes_mlvc`（验证 `-p neural` 路由到 `RKVC_CODEC_MLVC` + `RKVC_ENC_BACKEND_MLVC`）。
  - CLI 防护：`-p neural` 但输出非 `.mlvc` 时报错（MLVC 只能产出 `.mlvc` 码流）。
  - **用法**：`rkvc_transcode -i in.mp4 -o out.mlvc -p neural --mlvc-enc ... --mlvc-gaussian-pmf ... --mlvc-bitest-pmf ...`
  - **硬件验证**：`-p neural` 编码+解码往返 72 帧，码流字节一致（MD5 与 `-c mlvc -p balanced` 相同）；标准转码回归通过；`test_router` 7 项 + `test_internal` 10 项全过。


- **修复 MLVC 解码器 qp 硬编码 bug**（`lib/node_mlvc.c` / `lib/internal.h` / `lib/session.c`）：解码器此前 qp 硬编码为 21，从不读取 `.mlvc` 容器头中的实际 qp——导致 qp≠21 编码的码流无法解码（rANS 流去同步，约第 10 帧崩溃）。新增 `rkvc_mlvc_demux_qp()` 从容器头读取 qp，经 `rkvc_mlvc_dec_config.qp` 传入解码器。此前 qp=5/50 等仅能解码 10 帧即失败，现已全部修复（72 帧完整往返）。MLVC 的 qp 是与 bitrate/qp_init 平行的显式参数（`--mlvc-qp`，默认 21），与 policy 档位无关——policy 是编解码器选择器（realtime→264 / balanced→265 / quality→av1 / offline→av1-HQ），不是单个编解码器的质量旋钮；显式 `-c mlvc` 时 policy 被完全忽略，与 `-c h264` 等行为一致。

- **MLVC 语义质量档位**（`-p realtime|balanced|quality|offline` → qp）：MLVC 现在与其他编解码器一样参与 policy 档位系统，`-p` 自动映射到质量参数 qp，无需手动 `--mlvc-qp`。
  - **映射**（`router.c` `rkvc_mlvc_policy_qp()`）：`realtime`→qp=10 / `balanced`→qp=21（模型最优压缩点）/ `quality`→qp=30 / `offline`→qp=40。`--mlvc-qp N` 可覆盖。
  - **修复解码器 qp 硬编码 bug**（`node_mlvc.c`）：解码器此前 qp 硬编码为 21，从不读取容器头中的 qp——导致 qp≠21 编码的码流无法解码（流去同步）。现在 `rkvc_mlvc_demux_qp()` 从容器头读取 qp，经 `rkvc_mlvc_dec_config.qp` 传入解码器。此前 qp=5/50 等仅能解码 10 帧即失败，现已全部修复（72 帧完整往返）。
  - **硬件验证（RK3588）**：四档（realtime/balanced/quality/offline）编码+解码往返均 72 帧成功；qp=5/50 等自定义值也完整往返。标准转码（h264→hevc）回归通过；`test_internal` 10 项全过。


- **修正 MLVC 集成架构：从「转码中间件」改为与 264/265 平行的端到端编解码器**（`lib/session.c` / `cli/rkvc_transcode.c`）。此前 `.mlvc → 标准格式`时 session 强制改写路由为「MLVC 解码 + HEVC 再编码」（`session.c` 原 `enc_backend==MLVC && !output_is_mlvc` 分支），导致 MLVC 解码永远被有损重压成 HEVC、无法产出原始帧——MLVC 被当成转码中间件而非编解码器。本次按「编解码器独立选择」重构：解码后端←输入（`.mlvc`→MLVC 解码），编码后端←输出（`.mlvc`→MLVC 编码；`.yuv`→无编码器；标准容器→标准编码器）。
  - **三种一等操作**（与 264/265 对齐）：
    - **编码** `video → .mlvc`（`mpp → mlvc`，输出 MLVC 码流）。
    - **纯解码** `.mlvc → .yuv`（`mlvc → raw`，输出原始 NV12，**无再编码**）——此前无法实现，现在复用既有 `decode_loop`（FILE_DECODE 模板）直接写出。
    - **转码** `.mlvc → .mp4 -c hevc`（`mlvc → hevc_rkmpp`，MLVC 解码 + 标准编码，**需显式 `-c` 指定输出编解码器**）。
  - **`lib/session.c`**：`session_create` 中路由解析后按输入/输出方向修正 `dec/enc` 后端与显示名（移除原 `session_open_nodes` 中错误的强制 HEVC 改写）；新增 `session_is_raw_out()` 检测 `.yuv/.raw` 原始输出。
  - **`cli/rkvc_transcode.c`**：模板/编解码器选择改为方向驱动——`.mlvc` 输出→MLVC_STORAGE（编码）；`.mlvc` 输入+`.yuv` 输出→FILE_DECODE（纯解码）；`.mlvc` 输入+容器输出→FILE_TRANSCODE（转码）；新增防护：`-c mlvc` 但输出非 `.mlvc` 报错、`.mlvc` 解码缺 `--mlvc-dec` 报错。更新 `usage()` 文本说明三种操作。
  - **硬件验证（RK3588 NPU）**：编码 `mp4→.mlvc`（20 001 B，MD5 与此前一致）；纯解码 `.mlvc→.yuv`（72 帧 × 640×368 NV12 = 25 436 160 B，ffmpeg 可渲染，无 HEVC 参与）；转码 `.mlvc→.mp4 -c hevc`（72 帧 HEVC）；标准转码 `mp4→.mp4 -c hevc`（72 帧）回归通过；`test_internal` 10 项全过。


- **MLVC 熵编解码器改写为纯 C，彻底移除 C++17 与 `msrtc_rans`/`mlvc` 子模块依赖**（`lib/rans.h` + `lib/rans.c` 新增，`lib/node_mlvc.cpp` 删除）。此前 MLVC 后端依赖 `third_party/mlvc` 子模块中的 `msrtc_rans`（C++17 模板熵编码库，`EntropyCoder.cpp` 直接编译进 `librkvc`），迫使整个项目启用 C++ 语言与 `CMAKE_CXX_STANDARD 17`。本次将其完整移植为纯 C，项目回归单一 C 语言编译。
  - **新增 `lib/rans.h`（240 行）+ `lib/rans.c`（852 行）**：完整覆盖 msrtc_rans 全部能力——两种 rANS 变体（RansByte：uint32 state/uint8 unit；Rans64：uint64/uint32）、PMF→预计算编码符号表（定点倒数消除 Put 除法）、PMF→解码 CDF 累积频率表（二分查找）、流式编码/解码（多 coder 写入同一流）、一次性 Encode/Decode、Bypass 编解码（离群值）、可增长堆缓冲。算法来源 ryg_rans / msrtc_rans（MIT License，头文件保留 Microsoft 版权归属）。
  - **新增 `lib/node_mlvc.c`（~1090 行）**：将原 `node_mlvc.cpp`（C++17，使用 `std::vector`/`std::unique_ptr`/`msrtc_rans::`）重写为纯 C——容器改用 `rkvc_malloc`/`rkvc_free` + 手工数组，熵编解码改用 `rkvc_rans_*` C API。编码器逻辑保持不变（自身 `feature` 输出作下一帧参考，不运行 decoder NPU）。
  - **`CMakeLists.txt`**：`project(... LANGUAGES C)`（移除 CXX）、移除 `CMAKE_CXX_STANDARD`/`CMAKE_CXX_STANDARD_REQUIRED`、移除 `MLVC_RANS_DIR`/msrtc_rans 检测与 `RKVC_MLVC_INCLUDES`；`RKVC_MLVC_SRC` 改为 `lib/node_mlvc.c lib/rans.c`。`RKVC_ENABLE_MLVC` 选项保留，仍依赖 `RKVC_ENABLE_RKNN`。
  - **`third_party/mlvc` 不再是构建依赖**：源码与 CMake 均不再引用该子模块（仅 `rans.h/c` 头注保留算法来源归属）。新克隆无需 `git submodule update --init third_party/mlvc` 即可构建 MLVC。
  - **修复移植引入的关键 bug**：`enc_bypass()` 中逆序写入循环消耗了部分计数变量 `n`，导致 bypass 前缀/余数计算错误（仅当出现离群值触发 bypass 编码时影响码流）。改为在逆序写入前保留 `total_parts`。
  - **编译验证**：`gcc -std=c17 -Wall -Wextra` 零警告；`cmake --build` 全量编译通过（单一 C 语言，无 C++），`librkvc.so`/`librkvc.a` + `rkvc_transcode` 链接成功。
  - **硬件验证（RK3588 NPU）+ 码流兼容**：纯 C 版本编码输出与原 C++ 版本**逐字节一致**（640×368×72f mp4 → 20001 B `.mlvc`，MD5 `cf9c6c9a0347bdac1504c9666e989ace` 与旧版完全相同）；解码往返（`.mlvc` → 72 帧 HEVC mp4）有效。


- **C 语言标准升级 C11 → C17**（`CMakeLists.txt`：`CMAKE_C_STANDARD 11→17`）。C17（ISO/IEC 9899:2018）相对 C11 仅含勘误与缺陷报告修订、无新语言特性与库头，等同代码零改动；`__STDC_VERSION__` 由 `201112L` 升至 `201710L`。工具链 GCC 9.4.0（Ubuntu 20.04 aarch64）已实测 `-std=c17` 编译链接通过。无 ABI 影响。
- **集成 MLVC 神经视频编解码后端**（Microsoft [mlvc](https://github.com/microsoft/mlvc)）：将 MLVC 作为完整编解码器后端接入 router / pipeline / mux，通过 `RKVC_CODEC_MLVC` 选择。
  - **新增子模块** `third_party/mlvc`（`microsoft/mlvc` @ `e9f0114`，shallow）：主要取 `packages/msrtc_rans` 模块（rANS 熵编码 C++17 静态库，`EntropyCoder.cpp` 直接编译进 `librkvc`）。
  - **新增 CMake 选项** `RKVC_ENABLE_MLVC`（默认 ON）：启用 C++17，依赖 `RKVC_ENABLE_RKNN`；编译 `lib/node_mlvc.cpp` + msrtc_rans 源码进库。C↔C++ 边界经 `internal.h` 的 `extern "C"` 守卫。
  - **新增公共 API**（`policy.h` / `pipeline.h`）：`RKVC_CODEC_MLVC` codec 枚举、`RKVC_ENC/DEC_BACKEND_MLVC` 后端枚举、`RKVC_TEMPLATE_MLVC_STORAGE` 模板；`rkvc_pipeline_desc` 新增 `mlvc_enc/dec_model_path` / `mlvc_gaussian/bitest_pmf_path` / `mlvc_qp` 字段。
  - **新增 `lib/node_mlvc.cpp`（C++17, ~700 行）**：MLVC 编码器（YUV→encoder NPU→rANS 熵编码→码流；内部同步运行 decoder NPU 维护 ref 特征同步）、MLVC 解码器（码流→rANS→decoder NPU→YUV NV12）、自定义 `.mlvc` 容器 mux/demux（FFmpeg 无法封装原始 rANS 码流）；零拷贝 RKNN I/O（native NC1HWC2 fp16），PMF 表加载。
  - **路由/管线/会话集成**（`router.c` / `pipeline.c` / `session.c`）：全路径 MLVC 后端分发；自动检测 `.mlvc` 后缀切换 demux 路径；`session_downscale_for_encode` 已修正在 `enc_scale_denom≤1` 时仍按模型原生分辨率做 RGA 缩放。
  - **模型与 PMF 表**：`MLVCEncoder/Decoder_rk3588.rknn` + `gaussian/bitest.bin`（PMF1 格式）由用户提供，已加入 `.gitignore`。
  - **编译验证**：`librkvc.so` / `librkvc.a` 全量编译通过（C17 + C++17 混编），19 个 `rkvc_mlvc_*` 符号正确导出，msrtc_rans C++ 符号全部 local。
  - **CLI 支持**（`cli/rkvc_transcode.c`）：新增 `--codec mlvc` 及 `--mlvc-enc/--mlvc-dec/--mlvc-gaussian-pmf/--mlvc-bitest-pmf/--mlvc-qp` 选项；`.mlvc -> 标准格式` 自动路由为 MLVC 解码 + HEVC MPP 再编码（仅解码侧需 `--mlvc-dec` + PMF）。
  - **硬件验证（RK3588 NPU）**：编码（640x368x72f mp4 -> .mlvc）72 帧成功，20KB 输出（~2.2 kbps / 0.0094 bpp）；解码往返（.mlvc -> mp4）72 帧 HEVC 输出有效。修复 3 个 bug：初始化顺序 segfault、.mlvc magic 长度（5 非 6）、demux use-after-free（copy=1）。
  - **修复编码器参考帧同步逻辑**（源自 mlvc 官方源码确认）：编码器不再内部运行 decoder NPU。mlvc 的 `compress_core(get_recon=False)` 让编码器自身输出 `feature`（用量化后的 `y_hat` 计算，与解码器一致），作为下一帧参考——无需额外解码。编码器现在每帧仅 1 次 NPU 推理（原先为 2 次），`rkvc_mlvc_enc_config` 移除 `dec_model_path` 字段，CLI 编码侧不再需要 `--mlvc-dec`。
- **SVT-AV1 子模块升级 4.1.0 → 4.2.0**（`.gitmodules` / `third_party/SVT-AV1` @ `v4.2.0`）：跟踪上游点播/RTC 调优与 ARM 核优化；SOVERSION 仍为 `4`（`libSvtAv1Enc.so.4`），ffmpeg `libsvtav1` 与 `librkvc` 链接无需 ABI 改动。
  - **RD 与性能 A/B**（1080p 片段，`bench/ab_compare_svt.sh` → `bench/results/svt_ab_compare.csv`；解码/测质走项目 ffmpeg 的 `av1_rkmpp` 硬解 + `hwdownload`，与 `run_rd_benchmark.sh` 同一 RD 测质管线）：
    - **preset 11（rkvc `quality` 档）**：BD-rate(PSNR) **−2.21%**、BD-rate(SSIM) −0.50%；平均编码速度 **34.0→38.4 fps（+13.1%）**。
    - **preset 4（rkvc `offline` 档）**：RD 基本持平（BD-rate(PSNR) +0.67%，落在短片段测量噪声内）；平均编码速度 **3.1→3.3 fps（+4.7%）**。
  - **档位码率无需重新校准**：同 CRF 下两版实际码率漂移 <0.6%（preset 11 +0.53%、preset 4 −0.07%），`bench/config.json` 的 `calibration.svt_av1` 表（CRF→目标 kbps）原样沿用即可；对应上游「M3-M5 RA 预设调优」带来的码率优化在固定 CRF 下表现为质量增益而非档位偏移。
- **ffmpeg-rockchip 子模块升级至上游 `8.1` HEAD（`f66f2f8046` → `388741a354`，8.1.2，2026-07-18）**：上游 8.1 分支已 rebase，落后约 200 个提交（~3 个月）。涉及 `rkmppenc`/`rkmppdec` 大重构（编解码器框架 + 扩展 codec）、RKRGA 滤镜重构、新增 `MPP hwcontext` 与 `NV15`/`NV20` 比特流格式。
  - **项目 ROI 补丁兼容性**（`patches/ffmpeg-rockchip/0001-rkmppenc-roi-runtime-rc.patch`）：经核验在重构后的 `rkmppenc.c/h` 上**干净 apply + 编译通过**（225 行新增；patch 依赖的 `MPPEncFrame`/`mpp_sei_set` 上下文结构重构后仍保留）；无需改动。
  - **构建/链接**：`scripts/rebuild-ffmpeg-rkmpp.sh --clean` 全量重编通过；`libavcodec.so.62` 含 patch 烘焙的 ROI 符号（`rkvc_roi_force_intra` / `KEY_ROI_DATA` / `MppEncROICfg`）；libavcodec 主版本仍 `62`，`librkvc` 与全部 87 个目标链接无需改动。
  - **功能回归**（均通过）：四档 `rkvc_transcode` 编码（`realtime`→h264_rkmpp / `balanced`→hevc_rkmpp / `quality`+`offline`→SVT-AV1 v4.2.0）输出有效码流；SVT-AV1 码流经 `av1_rkmpp` 硬解验证（`ffmpeg_to_yuv420p_raw` IVF 分支 + `hwdownload`，与上一版解码帧逐位一致）；`rkvc_session_upscale`（av1_rkmpp 解码 + RGA 上采样）正常；RD bench 端到端（SVT 编码→av1_rkmpp 硬解→PSNR/SSIM 测质）通；16 个单元/CLI 测试全过（含 ROI 运行时；10 个 hardware/integration 会话测试需硬件采集/特定素材，CI 跳过）。
- **rockchip-mpp 子模块固定至上游 `1.1.0` tag（`c2c1ee50` → `c08762ebf`，2026-03-10 → 2026-08-11，约 5 个月 / 4557 个提交；较 `8f922ed3` 追加 1.1.0 发布的 11 个提交）**：区间内主要方向为 `mpp_dec`/`mpp_enc`/`mpp_buffer`/`buf_slot` 重构与 RC 调优、H.264/HEVC/JPEG 各 HAL 修复，以及上游新内核态 `kmpp` 框架推进（`kmpp_venc` ctrl/ref_cfg、`mpp_cfg_io` trie 序列化、`MppEncUserDataShm` 内联契约等，均不暴露于公共 API）。
  - **1.1.0 发布（2026-08-11，11 个提交）**：H.264 编码 vepu540c 空帧修复（issue #965）、H.264 解码大 NALU 按倍扩容、H.265/H.264 解码稳定性修复、kmpp legacy/obj 模式自动切换与 hal_info/kmpp_obj/mpp_trie 8 字节对齐修复。
  - **ABI 兼容，无需重编**：SOVERSION 保持 `1`（`librockchip_mpp.so.1`）；公共头文件（`inc/`）仅 8 文件小改（+83/−32），全部为新增能力——FBC layout 查询（`MPP_FRAME_FMT_IS_AFBC_V1/V2`、`mpp_frame_get/set_fmt_layout`）、编码统计字段（`KEY_ENC_MADI_B16` / `KEY_ENC_MADP_CTU`）、`rk_venc_kcfg` ref/ctrl cfg 扩展——无破坏性删除；项目不直接 include mpp 头（`lib/` 仅经 ffmpeg `rkmppenc`/`rkmppdec` 间接使用），`librkvc` 与全部目标链接无需改动。
  - **构建/运行验证**：`cmake --build .build/deps/mpp-build` 重编通过并安装至 `.build/deps/mpp-install`；既有 ffmpeg-rockchip（8.1，`388741a354`）与 `librkvc` 动态链接解析到新库（`ldd` 确认 `librockchip_mpp.so.1` → `.build/deps/mpp-install`）；冒烟：`rkvc_encode`（H.264 `realtime` 硬编 30 帧）→ `rkvc_decode` 全帧回读（320×240 NV12 3,456,000 B），日志确认 `mpp version: c08762ebf`（1.1.0）。

### 变更

- **DRM_PRIME 像素格式显式映射**（`lib/utils.c`）：会话解码（`av1_rkmpp` 等硬解输出 `AV_PIX_FMT_DRM_PRIME`=178 dmabuf 容器格式）经 `rkvc_from_av_pix_fmt()` 时此前落入 `default` 兜底分支，每帧输出误导性告警 `unknown AVPixelFormat 178, falling back to NV12`。经核 `AV_PIX_FMT_DRM_PRIME` 是 dmabuf 容器（实际像素格式在 `AVDRMFrameDescriptor` 中，本项目仅消费 `DRM_FORMAT_NV12`，由 `rkvc_buffer_from_drm_frame()` 校验并设 `format=NV12`），故显式 `case AV_PIX_FMT_DRM_PRIME → RKVC_PIX_FMT_NV12`，与既有 dmabuf 路径行为一致、消除告警。此为既有缺漏（与本次升级无关，DRM_PRIME=178 枚举稳定），借升级核验一并收敛兜底。

- **SVT-AV1 编码器 flush 丢尾包修复**（`lib/session.c` `session_flush_encoder`）：`806de47`（2026-07-14「修复 DMABUF 内存泄漏」）将 flush 循环中 `RKVC_ERR_AGAIN` 的处理由 `continue` 改为 `break`（意图避免空转），但破坏了 FFmpeg `libsvtav1` 的 flush 协议——收尾阶段 `eb_receive_packet` 对空 SVT 输出队列返回 EAGAIN（`EB_NoErrorEmptyQueue`），此时 EOS 包尚未产出，遇 EAGAIN 即 break 会丢尾包。SVT preset 11（带前瞻/重排）实测出帧不稳（90 帧入，39~54 抖动）。改为 EAGAIN 时退让 `100µs` 后 `continue`，收到有效包则继续，直至真正 EOF。隔离验证（`rkvc_encode` 直喂 raw YUV，不经解码）连续 3 次出帧稳定 90/90。

- **rkmpp 解码 EAGAIN 丢包修复**（`lib/session.c` / `lib/node_mpp_dec.c`）：`transcode_loop`、普通文件解码与 AI-SR 解码原先均在 `avcodec_send_packet()` 返回 EAGAIN 后立即释放压缩包，下一轮继续读取新包，违反“未接收即重试同一输入”的 send/receive 所有权协议，造成 HEVC/AV1 等硬解随机缺帧及 `missing ref poc`。新增统一 decoder pump：pending packet 在 send 成功前保持引用；rkmpp 异步双 EAGAIN 时退让 `100µs` 后重试；demux EOF 仅在 pending packet 清空后提交 drain；NULL packet 仅在解码器确认接收后标记完成。回归结果：H.264 372 帧连续 3 次均 `372/372`，HEVC `1009/1009`，AV1 `240/240`。

- **编码 send/flush 协议加固**（`lib/session.c` / `lib/node_mpp_enc.c` / `lib/node_svt_enc.c`）：MPP 与 SVT 编码器遇 send EAGAIN 时均排空输出并重试同一帧；帧 PTS 仅在 send 真正成功后递增；drain 的 NULL frame 仅在编码器确认接收后设置 flushed，避免同类丢帧、时间戳空洞或未实际发送 EOS。硬件集成测试同步从“仅检查返回码”收紧为输入 packet、解码帧与编码输出帧精确相等。


- **SVT-AV1 子模块升级 4.1.0 → 4.2.0**（`.gitmodules` / `third_party/SVT-AV1` @ `v4.2.0`）：跟踪上游点播/RTC 调优与 ARM 核优化；SOVERSION 仍为 `4`（`libSvtAv1Enc.so.4`），ffmpeg `libsvtav1` 与 `librkvc` 链接无需 ABI 改动。
  - **RD 与性能 A/B**（1080p 片段，`bench/ab_compare_svt.sh` → `bench/results/svt_ab_compare.csv`；解码/测质走项目 ffmpeg 的 `av1_rkmpp` 硬解 + `hwdownload`，与 `run_rd_benchmark.sh` 同一 RD 测质管线）：
    - **preset 11（rkvc `quality` 档）**：BD-rate(PSNR) **−2.21%**、BD-rate(SSIM) −0.50%；平均编码速度 **34.0→38.4 fps（+13.1%）**。
    - **preset 4（rkvc `offline` 档）**：RD 基本持平（BD-rate(PSNR) +0.67%，落在短片段测量噪声内）；平均编码速度 **3.1→3.3 fps（+4.7%）**。
  - **档位码率无需重新校准**：同 CRF 下两版实际码率漂移 <0.6%（preset 11 +0.53%、preset 4 −0.07%），`bench/config.json` 的 `calibration.svt_av1` 表（CRF→目标 kbps）原样沿用即可；对应上游「M3-M5 RA 预设调优」带来的码率优化在固定 CRF 下表现为质量增益而非档位偏移。
  - **新增 bench 脚本** `bench/ab_compare_svt.sh`：SVT 版本 A/B 对比（同片段、同 CRF 阶梯，preset 11/4），输出码率/PSNR/SSIM/编码耗时 CSV；测质解码用项目 ffmpeg 的 `av1_rkmpp` 硬解 + `hwdownload`（与 `run_rd_benchmark.sh` 的 `ffmpeg_to_yuv420p_raw()` IVF 分支同一管线），经核对与系统 `libaom-av1` 软解的 PSNR/SSIM 逐位一致（AV1 解码确定性，硬解软解同帧）。
  - **VPU 硬解核查**：`av1_rkmpp` AV1 硬解在本机工作正常（`/proc/mpp/` 注册 `av1d` 客户端，IVF→`-c:v av1_rkmpp -vf hwdownload,format=nv12`→rawvideo 解出完整帧，实测 v4.1.0/v4.2.0 两版解码 PSNR 与 `libaom` 完全一致）；RD 测质脚本中曾出现的 `ENOSYS`（Function not implemented）为早期调用形态缺 `-vf hwdownload`（DRM_PRIME 帧无法落系统内存）所致，非 VPU 硬件故障，已对齐 bench 测质管线修复。

## [0.2.8] - 2026-08-05

### 新增

- **授权指纹容器环境加固**（`lib/license_machine.c`）：检测到容器环境（`/.dockerenv`、`/run/.containerenv` 或 PID 1 cgroup 含 docker/kubepods/containerd/libpod/lxc 特征）时拒绝 MAC 兜底指纹——容器 MAC 随实例重建而变且多容器可同 MAC，无法稳定绑定单机；设备树/OTP 指纹不受影响。确需在容器内使用 MAC 授权时可设 `RKVC_LICENSE_ALLOW_CONTAINER_MAC=1` 显式放行。
- **机器码分组显示**（`lic_machine_id_grouped()` / `rkvc_lic machine-id`）：诊断输出新增 4 字符分组形式（`xxxx-xxxx-…`），便于人工报码/抄码；stdout 原始 hex 不变，脚本用法兼容。

### 变更

- **开源协议切换为 AGPLv3**（[LICENSE](LICENSE)）：由 MIT 改为 GNU Affero General Public License v3。衍生/合并作品须以 AGPLv3 开源，网络服务须向用户提供源码；闭源商业使用需商业授权（与 `RKVC_ENABLE_LICENSE` 授权机制配套）。
- **FFmpeg 构建改为 LGPLv3**（`scripts/rebuild-ffmpeg-rkmpp.sh` / `scripts/rebuild-ffmpeg-av1.sh`）：移除 `--enable-gpl --enable-nonfree`，保留 `--enable-version3`，消除与 AGPLv3 的许可冲突（AGPLv3 与 LGPLv3 兼容）。
- **发行包补发许可证文本**（`scripts/package-portable.sh`）：AGPLv3 与各第三方组件许可证（ffmpeg、SVT-AV1、librga、mpp、libsodium）随包分发至 `licenses/`，满足 AGPL §4 及 Apache/BSD/ISC 的再分发义务。
- **许可证合规整改**（独立审查后补强）：
  - **双许可声明**（[LICENSE](LICENSE) / [README.md](README.md)）：正式声明「AGPLv3 开源 + 商业授权」双许可，明确 AGPL 版（`RKVC_ENABLE_LICENSE=OFF`，默认）无附加限制，强制授权仅存在于商业授权构建。
  - **发行包补全第三方许可**（`scripts/package-portable.sh`）：`licenses/` 加入 SVT-AV1 `PATENTS.md`（AOM 专利许可 1.0）；新增 `licenses/ffmpeg-modifications/`，随包附 FFmpeg 修改补丁与对应源码说明（子模块 commit / URL / configure 参数），满足 LGPLv3 §4 修改版本源码义务。
  - **文档修正与来源声明**（[docs/packaging.md](docs/packaging.md) / [docs/delivery.md](docs/delivery.md)）：纠正 SVT-AV1 许可为 BSD-3-Clause Clear + AOM 专利许可（原误标 BSD-2）；补全第三方许可表（librga / libsodium / 本项目）；新增模型（自训练、加密交付范围）与 `librknnrt`（Rockchip 专有、再分发须遵守 SDK 条款）来源声明。
  - **bench 许可证标注**：9 个 Python 脚本补 `SPDX-License-Identifier: AGPL-3.0-or-later`；`bench/pyproject.toml` 补 `license` 字段。
- **clang-tidy 机制收敛**（`CMakeLists.txt` / `CMakePresets.json`）：删除 `RKVC_ENABLE_TIDY` option 与自定义 `tidy` target 两条冗余路径，仅保留 `tidy` preset（`CMAKE_C_CLANG_TIDY`）一种用法。
- **`full-tests` preset 并入 `tests`**（`CMakePresets.json`）：`tests` 显式开启 `RKVC_BUILD_CLI`，CLI 脚本用例（`test_cli_args` / `test_bench_permission_failure`）与单元测试同树运行（19 个 CTest 目标）；原 `.build/full-tests/` 目录废弃。
- **install 规则常规定义**（`CMakeLists.txt`）：移除从未声明的幻影变量 `RKVC_INSTALL` 守卫，install 规则始终定义（仅 `cmake --install` 时生效），与 `docs/getting-started.md` 记载行为一致。

### 移除

- **死代码与重复路径清理**（依 AGENTS.md 原则审计）：
  - 零调用公共 API：`rkvc_hash_file()`（`include/rkvc/rkvc.h` / `lib/ffmpeg_util.c`）、`rkvc_net_send_buffer()`（`include/rkvc/net.h` / `lib/net.c`）。
  - 从未被读取的 `rkvc_pipeline_desc.b_frames` 字段（`include/rkvc/pipeline.h`）：RKMPP 编码器固定 `max_b_frames = 0`，SVT 路径亦不消费。
  - 三组重复示例 `stream_decode` / `stream_transcode` / `stream_encode`（与 `decode_file` / `transcode` / `encode_file` 功能重复）及占位示例 `psnr_test`（不做 PSNR，仅打印 caps）。
  - `scripts/run-bench.sh` 转发脚本：RD 基准入口统一为 `bench/run_rd_benchmark.sh`（`RKVC_BUILD` 默认值本就由 `bench/config.json` 提供），CMake `bench-rd` 目标与全部文档同步指向真实脚本。
  - 自研 base64（`lib/license_b64.c/h`）：注册码编解码统一改用 libsodium `sodium_base642bin()` / `sodium_bin2base64()`（`lib/license.c` / `tools/rkvc_lic.c` / `scripts/package-portable.sh` 同步），签发端与校验端共享同一成熟实现；同时收紧校验，不再容忍 URL-safe 字母表（签发端从未产生）。
  - CMake 侧 `RKVC_BUILD_JOBS_MAX` 推测性旋钮（各 build preset 已硬编码 `jobs: 4`；`scripts/build-common.sh` 的同名环境变量保留）。

### 修复

- **`test_bench_permission_failure.sh` 自 0.2.0 起失效**：原场景 `rkvc_transcode -i /dev/null` 在 demux 阶段即失败，永远到不了 dma_heap 权限拒绝路径（且 `rkvc_transcode` 不打印错误串）；CI 因 fault injection 关闭走 exit 77 跳过而未暴露。改用 `rkvc_encode`（`FILE_ENCODE` 在 `rkvc_session_create` 即打开编码器并分配 dma_heap，CLI 打印 `rkvc_err_str`），权限拒绝传播链路恢复真实覆盖。

## [0.2.7] - 2026-07-15

### 发布重点

rkvc **0.2.7** 新增可选的 **1机1码离线授权**（Ed25519 非对称签名 + 硬件指纹），库内嵌公钥本地校验，无需联网；开启后 `rkvc_init()` 强制校验，无有效授权则拒绝初始化。同时修复 **DMABUF 自分配缓冲的 mmap 内存泄漏**、**muxer 写头失败后 `av_write_trailer` 崩溃**、**NV12 写盘 `fwrite` 返回值被忽略**、**编码器 flush 遇到 `AGAIN` 时空转**，以及测试二进制直接运行时找不到传递依赖库的问题。授权模块默认关闭，开启不影响既有 API。

### 新增

- **1机1码离线授权**（`include/rkvc/license.h` / `lib/license.c`）：基于 Ed25519 非对称签名实现离线授权。机器码由本机硬件指纹（设备树序列号 -> Rockchip OTP -> 网卡 MAC）经 SHA-256 派生为 64 字符十六进制串；发码端用私钥对「magic + product + 机器码」做 Ed25519 签名，库内嵌公钥本地验签，私钥离线保管，攻击者拿到公钥也无法伪造注册码。授权一经签发永久有效，不含有效期字段、不做到期/防时间回拨校验。
  - 许可证二进制布局 104 字节（小端）：`magic(4) | product_id(4) | machine_id[32] | signature[64]`，签名覆盖前 40 字节；注册码 = base64(blob)，授权文件为注册码文本。
  - 公共 API：`rkvc_machine_id()`（采集机器码）、`rkvc_license_verify_file()` / `rkvc_license_verify_blob()`（校验签名 + 机器码匹配）、`rkvc_license_default_path()` / `rkvc_license_check()`（默认路径便捷校验）；`rkvc_license_info` 回填解析详情。
  - 默认授权文件查找顺序：环境变量 `RKVC_LICENSE_FILE` -> `~/.config/rkvc/license.lic`。
- **libsodium 子模块**（`third_party/libsodium`）：提供 Ed25519 签名/验签与 SHA-256。选用 libsodium 因其原生支持 pure EdDSA（mbedTLS 3.6/4.1 仅有声明无实现）；沿用本项目对非 CMake 子模块的「install 脚本 + 前缀」惯例，静态链接，部署无需额外 `.so`。
- **`rkvc_lic` 签发/管理工具**（`tools/rkvc_lic.c`）：独立于 librkvc，仅依赖 libsodium。子命令：`genkey`（生成 Ed25519 密钥对）、`machine-id`（打印本机机器码）、`issue`（签发注册码）、`inspect`（解析字段）、`verify`（校验签名 + 机器码）。
- **`scripts/install-libsodium.sh`**：从子模块源码 autotools 构建并安装静态库到 `.build/deps/libsodium-install`（需 `autoconf` / `automake` / `libtool`）。
- **`docs/license.md`**：授权原理、构建、签发流程与密钥管理文档。
- **错误码**（`include/rkvc/types.h`）：`RKVC_ERR_LICENSE`（-12，授权校验失败）、`RKVC_ERR_UNLICENSED`（-13，未找到授权）。

### 变更

- **CMake 授权选项**（`CMakeLists.txt`）：新增 `RKVC_ENABLE_LICENSE`（默认 OFF）。开启即编译 `lib/license.c` + 链接 libsodium，并在 `rkvc_init()` 运行时强制校验。公钥来源二选一：`RKVC_LICENSE_PUBKEY_FILE` 指向 32 字节公钥二进制文件时 CMake 自动生成 `license_pubkey.c`（生产推荐）；未设置则用 `lib/license_pubkey.c` 演示公钥并发出 WARNING。
- **`rkvc_init()` 强制校验**（`lib/init.c`）：`RKVC_ENABLE_LICENSE` 开启时，`rkvc_init()` 首先调用 `rkvc_license_check()`，失败则记录日志并拒绝初始化（返回对应错误码），保证未授权设备无法使用库。
- **`rkvc.h`** 纳入 `license.h`；`rkvc_err_str()` 补齐三个授权错误码描述。
- **可移植包 `--license` 模式**（`scripts/package-portable.sh`）：新增 `--license` 选项，自动完成全部密钥管理——构建 libsodium、编译临时 `rkvc_lic`、检查/自动生成密钥对（首次）、用公钥注入 CMake 编译（`RKVC_LICENSE_PUBKEY_FILE`）、签发本机自测 license（不随包分发）、打包 `rkvc_lic` 到 `bin/`。成品包名后缀 `-licensed`，目标机须放置有效 license 后方可运行。`docs/packaging.md` 同步新增授权构建章节、密钥管理表与客户签发流程。
- **测试二进制依赖库路径**（`scripts/build-common.sh` / `test-npu-sr.sh` / `test-rga.sh`）：`test_*` 二进制将依赖库写入 `DT_RUNPATH`，但 `DT_RUNPATH` 不解析传递依赖（如 `libavcodec.so` -> `libSvtAv1Enc.so.4`），导致不经 ctest 直接运行 `test_*` 时找不到依赖库。新增 `rkvc_dep_library_path()` 返回与 CMake `RKVC_DEP_LIB_DIRS` 一致的动态库路径（ffmpeg 子目录 / mpp / SVT-AV1 / librga 安装前缀），在两个测试脚本中经 `LD_LIBRARY_PATH` 注入；ctest 自行注入故无需改动。
- `.gitignore`：忽略 `*.docx` 办公文档；忽略授权私钥与 license 文件（`*.pem` / `*.lic` / `tools/keys/*secret*` / `tools/keys/*.pem` / `tools/keys/public_key.h`，切勿提交）。
- `.gitmodules`：新增 `third_party/libsodium`（shallow）。

### 修复

- **DMABUF 内存泄漏**（`lib/buffer_pool.c` / `lib/internal.h`）：`rkvc_buffer` 新增 `mmap_base` / `mmap_size` 字段记录 dma-heap 自分配缓冲的 mmap 基址与大小，由 `buffer_free` 统一执行 `munmap` + `close(fd)`。此前释放逻辑以 `AV_PIX_FMT_DRM_PRIME` 区分 RKMPP DRM 帧与 dma-heap 自分配缓冲并据此决定是否 `close(fd)`，但 `close(fd)` 不会解除映射，dma-heap 缓冲的 mmap 内存从未被 `munmap`；而 `av_frame_alloc` / `av_image_fill_arrays` 失败路径上的手动 `munmap` 在移交所有权后已属冗余。重构后以 `mmap_base` 非空作为自分配缓冲的唯一判据，覆盖分配失败与正常释放全路径，消除长时运行泄漏。
- **muxer 写头失败崩溃**（`lib/node_mux.c`）：新增 `header_written` 标志，仅在 `av_write_header` 成功后才在 `rkvc_mux_close` 中调用 `av_write_trailer`，防止写头失败后对未初始化的 muxer 上下文调用 trailer 造成崩溃。
- **NV12 写盘错误静默忽略**（`lib/session.c`）：`session_write_nv12_frame` / `session_write_nv12_buffer` 由 `void` 改为返回 `rkvc_err`，所有 `fwrite` 路径检查返回值并向上传播；`decode_loop` 写盘失败时关闭文件并返回错误，避免磁盘满 / I/O 错误时静默截断输出。
- **编码器 flush 空转**（`lib/session.c`）：`session_flush_encoder` 中 flush 遇到 `RKVC_ERR_AGAIN` 时由 `continue` 改为 `break` 退出循环，避免在编码器无法产出更多包时无限重试空转。

### 测试

- 版本号升至 **0.2.7**（`CMakeLists.txt` `project(VERSION)` 为唯一来源）；编译零警告。授权模块默认关闭，不影响既有 21 项 CTest。

## [0.2.6] - 2026-07-14

### 发布重点

rkvc **0.2.6** 修复代码审查发现的线程安全与诊断缺陷：**计时统计数据竞争**（解码循环中四个 `*_sec` 字段此前无锁写入）、**`stop_requested` 跨线程可见性**、**输出端口溢出无丢弃统计**，以及 **hw 设备/像素格式回退静默** 与 **`av_init_packet` 弃用调用**。均为防御性加固，无 API 变更，无行为回归。

### 修复

- **计时统计数据竞争**：`decode_loop` / `decode_loop_ai_sr` 中 `decode_sec` / `rga_sec` / `write_sec` / `postproc_sec` 此前在工作线程**无锁写入**，而 `rkvc_session_get_stats` 在锁内读取。新增 `rkvc_session_stats_add_timing()` / `rkvc_session_stats_reset_timing()`（`scheduler.c`），全部写入统一经 mutex 保护，与 `frames_in` / `frames_out` 计数器一致。
- **`stop_requested` 可见性**：改为 `volatile int`（`internal.h`），防止编译器在 `while (!stop_requested)` 轮询中缓存寄存器值。
- **`start` / `stop` 加锁**：`running` / `stats.running` 更新纳入 `session->lock`。
- **输出端口溢出统计**：新增 `session_push_output()`（`session.c`），`output` 端口队列满时计入 `frames_dropped`（mux 写盘不受影响）。

### 变更

- **hw 设备错误诊断**（`node_mpp_enc.c`）：RKMPP hw 设备初始化失败时输出 `AV_LOG_WARNING`，替换原先的 `(void)herr` 静默丢弃。
- **像素格式回退日志**（`utils.c`）：`rkvc_from_av_pix_fmt` 遇到未知 `AVPixelFormat` 回退 NV12 时输出 WARNING。
- **`av_init_packet` 弃用替换**（`node_mpp_dec.c`）：栈上 `AVPacket` + `av_init_packet` 改为 `av_packet_alloc` / `av_packet_free`（堆分配引用计数对象），消除 `-Wdeprecated-declarations` 警告并面向未来 FFmpeg ABI 解耦。
- **编码器收尾去重**（`session.c`）：`transcode_loop` / `live_capture_loop` / `encode_file_loop` 三处 ~15 行逐字相同的 drain + flush 尾部块抽取为 `session_flush_encoder()`，并新增 `session_receive_packet()` 收敛 9 处 `if (s->enc) ... else svt` 后端分派；`session.c` 减少 ~40 行。
- **码流包构造去重**（`buffer_pool.c` / `node_demux.c` / `node_mpp_enc.c` / `node_svt_enc.c`）：三处逐字相同的 ~14 行 `AVPacket → rkvc_buffer` 手动构造抽取为 `rkvc_buffer_from_avpacket()`；两份复制的 `host_frame_from_buffer()` 合并为 `rkvc_buffer_to_host_frame()`。
- **CI 链接修复**（`CMakeLists.txt`）：`FFMPEG_LIBS` 增加 `SvtAv1Enc`；静态 `libavcodec.a` 内的 `libsvtav1` 编码器引用 `svt_av1_enc_*` 符号，此前未显式链接导致 `librkvc.so` 及 examples 链接失败（`undefined reference`）。
- **子模块补丁自动还原**（`scripts/build-common.sh`）：`rkvc_apply_ffmpeg_patches` 成功应用补丁后注册 EXIT 陷阱，脚本退出（成功或失败）时由新增 `rkvc_restore_ffmpeg_clean` 逆序反向应用补丁，`ffmpeg-rockchip` 子模块工作区始终恢复干净状态，不再残留 `-dirty` gitlink；`patches/ffmpeg-rockchip/README.md` 同步说明。

### 测试

- 版本号升至 **0.2.6**；编译零警告（此前唯一的 `av_init_packet` 弃用警告已消除）。
- 21 项 CTest 通过（12 单元 + 9 硬件跳过）；`test_net` 在禁用 socket 的沙箱环境中失败（环境限制，非代码缺陷）。

## [0.2.5] - 2026-07-10

### 发布重点

rkvc **0.2.5** 落地在线采集与智能压缩原语：**V4L2 `LIVE_CAPTURE`**、**ROI（MPP 硬区域 QP）**、**进程级多 Session 配额**、**MPP 硬 ROI 桥接**（ffmpeg-rockchip `rkmppenc`）、**UDP/RTP 码流收发原语**，以及 **运行中热切换**（码率/GOP/IDR）与 **`preview` 端口接线**。

### 新增

- **`node_v4l2`**：NV12 MPLANE/单平面采集；`pipeline_desc.capture_device` / `capture_max_frames` / `capture_timeout_ms`。
- **`LIVE_CAPTURE`**：`rkvc_session_run_file` 走 `live_capture_loop`；采集帧同时推 `capture` 与 `preview`（满则丢最旧）；示例 `example_live_capture`、`example_stream_device_pair`。
- **ROI API**（`include/rkvc/roi.h`）：`rkvc_session_set_roi` / `clear_roi`。
- **MPP 硬 ROI**：`rkmppenc` 读取 `AV_FRAME_DATA_REGIONS_OF_INTEREST` → `KEY_ROI_DATA`；`force_intra` 经帧 metadata `rkvc_roi_force_intra`；`rkvc_mpp_enc_send_frame_roi`；仅 H.264/HEVC；SVT 忽略 ROI（无像素 fallback）。
- **Runtime 配额**（`include/rkvc/runtime.h`）：`max_sessions` / `max_enc_sessions` / `max_npu_sessions`；超限 `rkvc_session_create` → `RKVC_ERR_AGAIN`。
- **`include/rkvc/net.h` / `lib/net.c`**：`rkvc_net_open/send/recv/finish`；UDP 16B 分片头（最多 16 片）；RTP over UDP（PT=96，Marker 帧尾）。
- **热切换**（`include/rkvc/reconfig.h`）：`set_bitrate` / `set_gop` / `request_idr` / `reconfigure`；MPP 经 `rkmppenc` 运行时 `MPP_ENC_SET_CFG`。
- **V4L2 mock**：`capture_device="mock"` 合成 NV12；单元测试 `test_v4l2`（节点级始终跑；Session 短录需硬件标志）。
- **示例** `example_net_loopback`；单元测试 `test_roi_runtime`、`test_reconfig`、`test_v4l2`、`test_net`。
- **`network-e2e-test.sh`**：调用 `example_net_loopback` 做 UDP+RTP 冒烟。

### 变更

- 版本号升至 **0.2.5**。
- `rkvc.h` 纳入 `roi.h` / `runtime.h` / `net.h` / `reconfig.h`。
- 子模块补丁：`patches/ffmpeg-rockchip/0001-rkmppenc-roi-runtime-rc.patch`（由 `rebuild-ffmpeg-rkmpp.sh` 构建前幂等应用；含硬 ROI + 运行时 RC）。
- **ffmpeg-rockchip 切至 `8.1` 分支**；AV1 编码经 FFmpeg **`libsvtav1`**（不再直连 `EbSvtAv1Enc`）；`third_party/SVT-AV1` 仍为构建/运行时依赖。

## [0.2.4] - 2026-07-09

### 发布重点

rkvc **0.2.4** 补齐第四档语义策略 **`OFFLINE`**（非实时高质量：SVT-AV1 preset 4 + `av1_rkmpp`，目标 ≥1 fps@1080p），并在 RD bench 中对齐 `svt-av1-hq` / `rkvc-offline` 与 HQ 下采样超分路线。同时清掉全部 **v1 兼容残留**（API 别名、无效 CLI 开关、迁移文档与旧 bench 命名），Session / Router 成为唯一公开面。

### 新增

- **语义档位 `OFFLINE`**：`RKVC_POLICY_OFFLINE`；CLI `-p offline`（`rkvc_encode` / `rkvc_transcode`）；Router 映射为 SVT-AV1 preset 4 + `av1_rkmpp`；内部常量 `RKVC_SVT_PRESET_HQ`。
- **`rkvc_bench` 四策略**：在 REALTIME / BALANCED / QUALITY 之外增加 `OFFLINE (AV1 HQ)`；可移植包自测同步校验四档 fps。
- **RD bench 高质量基线**：`svt.hq_preset`（默认 4）/ `SVT_HQ_PRESET`；路线 `svt-av1-hq`、`rkvc-offline`；支持 `svt-av1-hq+up{N}x-{bilinear,rkvc_sr}`。
- **绘图**：`svt-av1-hq` 独立深绿配色；历史 CSV 别名 `mobileone` 按 AI 超分处理；曲线 z-order 为 RGA → AI → 全分辨率基线 → HQ / rkvc 最上。

### 变更

- 版本号升至 **0.2.4**（`CMakeLists.txt` `project(VERSION)` 为唯一来源）。
- Bench 默认路线：`rkvc-v2` → `rkvc`（展开含 `offline`）；prep 键 `rga-bilinear-nv12-v1` → `rga-bilinear-nv12`（旧 prep 缓存需重建一次）。
- `QUALITY` 文档语义明确为近实时（SVT preset 11）；离线归档改走 `OFFLINE`。
- 架构 / 交付 / 基准文档与 `examples/transcode.c` 同步四策略说明。

### 移除

- 公共 API：`rkvc_preset` / `RKVC_PRESET_*`、`RKRC_*` 码控别名、`rkvc_is_valid_preset()`。
- `rkvc_encode --post-upscale`（编码路径只做下采样；上采样请用 `rkvc_session_upscale`）。
- `docs/migration.md` 及站点 / README / 交付文档中的 v1 迁移入口。

### 测试

- `test_router`：`test_offline_routes_av1_hq`（preset 4）。
- 硬件用例：`test_session_transcode_offline`（`RKVC_RUN_HARDWARE_TESTS=1`）。
- 可移植包：`rkvc_bench` 四策略短测（拒绝 `-1.0 fps`）。
- Bench 实机：`svt-av1-hq` 全分辨率，以及 `+up3x-bilinear` / `+up3x-rkvc_sr` RD 扫点。

## [0.2.3] - 2026-07-09

### 发布重点

rkvc **0.2.3** 补齐 **NPU / `rkvc_sr` 完整测试门禁**，可移植包在携带 `librknnrt.so` 之外再附带约定超分模型；构建产物统一收纳到 **`.build/`**；**librga** 改为 `third_party/librga` 子模块（浅克隆），不再依赖开发板系统预装。目标板仍需 NPU 驱动/固件与 `/dev/rga` 设备节点。

### 变更

- 版本号升至 **0.2.3**（`CMakeLists.txt` `project(VERSION)` 为唯一来源）。
- **源码目录重命名**：`tools/` → `cli/`（正式命令行入口；与 `bench/tools/` 辅助脚本区分）。
- **CMake 选项重命名**：`RKVC_BUILD_TOOLS` → `RKVC_BUILD_CLI`（内部目标列表 `RKVC_CLI_TARGETS`）。
- **`rkvc_caps.has_rknn`** / **`rkvc_info --json` `rknn`**：RKNN 已编译且 NPU 可访问时置位。
- **`rknn_query(RKNN_QUERY_SDK_VERSION)`**：`rkvc_sr` 初始化日志打印 `rknnrt api=` / `drv=`。
- **硬件用例** `test_session_encode_decode_upscale_3x_ai_sr`（`RKVC_RUN_HARDWARE_TESTS=1`；模型默认 `models/rkvc_sr_x3.crypt.rknn`，可用 `RKVC_SR_MODEL` 覆盖）。
- **`scripts/test-npu-sr.sh`**：仿 RGA 门禁的 NPU AI 超分推广脚本（测试树 `.build/tests/`）。
- **`scripts/package-portable.sh`**：启用 RKNN 时拷贝 `models/rkvc_sr_x3.crypt.rknn` 进包内 `models/`；并随包携带 `librga.so`。
- **`scripts/test-portable.sh`** / **`portable-test-helpers.sh`**：校验包内模型与 `librga`；有 NPU 时跑 `rkvc_sr` 冒烟。
- Bench 默认模型路径改为 `models/rkvc_sr_x3.crypt.rknn`。
- **构建目录收纳到 `.build/`**：约定见 `docs/build-layout.md` / `CMakePresets.json`（`release` / `tests` / `deps` / `portable` / `dist` 等）；根目录不再并列 `build*`；新增 `portable` preset。
- **`third_party/librga` 子模块**（[airockchip/librga](https://github.com/airockchip/librga)）：`install-librga.sh` 默认装到 `.build/deps/librga-install/`；CMake / ffmpeg 重建 / 可移植包均走该前缀，不再取系统预装库。
- **所有子模块浅克隆**：`.gitmodules` 为 ffmpeg-rockchip / mpp / SVT-AV1 / librga 设置 `shallow = true`；文档与 CI 统一 `git submodule update --init --depth 1`。

### 测试

- **NPU 门禁**：`./scripts/test-npu-sr.sh` 实机通过（硬件用例 + session smoke）。
- **可移植包**：`rkvc-*-linux-aarch64-portable.tar.gz`（含 `lib/librga.so` + `lib/librknnrt.so` + `models/`）。

## [0.2.2] - 2026-07-09

### 发布重点

rkvc **0.2.2** 将 **RKNN 运行时 `librknnrt.so`** 纳入可移植包，使 `rkvc_sr` AI 超分路径在目标板上不再依赖系统预装该库（仍需 NPU 驱动/固件；`librga` 仍为系统可选依赖）。

### 变更

- 版本号升至 **0.2.2**（`CMakeLists.txt` `project(VERSION)` 为唯一来源）。
- **`scripts/package-portable.sh`**：若 `librkvc` 链接了 `librknnrt.so`，则拷贝进包内 `lib/`，并纳入自包含校验。
- **`scripts/test-portable.sh`**：校验 `librknnrt` 解析到包内 `lib/`。
- **版本去硬编码**：`scripts/build-common.sh` 提供 `rkvc_project_version` / `rkvc_portable_pkg_dir`；CI 与文档示例改用通配符或 helper，不再写死当前版本；`scripts/check-version-literals.sh` 防回归。

### 测试

- **可移植包**：`rkvc-*-linux-aarch64-portable.tar.gz` 解压后 `./test.sh` **99 项 / 0 失败**。

## [0.2.1] - 2026-07-02

### 发布重点

rkvc **0.2.1** 在 v2 Session 管线之上补齐 **RKNN 神经网络超分（`rkvc_sr`）**、重构 **RD 基准测试配置与绘图**，并强化 **可移植包与硬件测试** 门禁。后处理上采样从「RGA 三档插值」扩展为可选 **AI 超分**；bench 支持 **SVT-AV1 内建 superres** 实验路线与 **左右对比演示视频** 批量生成。

- **RKVC SR**：`post_upscale_algo=rkvc_sr` + `post_upscale_rkvc_model_path`，RKNN 双缓冲异步推理，NEON 量化/反量化，RGA CSC（NV12↔RGB）；`rkvc_session_upscale --post-upscale rkvc_sr --rkvc-sr-model PATH`。
- **Bench 配置化**：`bench/config.json` 集中路径、码率扫点、RD 校准表与路线开关；`bench/tools/config.py` 校验/导出；`BENCH_CSV_MODE=session` 默认仅保留本次跑分 codec。
- **可移植包**：`test.sh` **99 项**（+7：三策略 bench 严格匹配、`rkvc_session_upscale` 2× 上采样）；CI `package` job 新增 portable 构建与自测。
- **演示管线**：`bench/tools/comparison_demo_rkvc.py` + `scripts/make-comparison-demo.sh`，生成「1080p AV1 参考 | 低码率 AV1 + RKVC SR 3× 还原」左右对比片。

### 新增

- **RKNN 超分节点**
  - `lib/node_rkvc_sr.c`：明文/加密 `.rknn` 模型加载、双 slot 异步推理、RGA 下采样预处理 + NEON int8 量化。
  - `lib/rkvc_sr_neon.c`：RGB24 NHWC ↔ NCHW int8 向量化转换。
  - 公共枚举 `RKVC_UPSCALE_AI_SR`；`rkvc_pipeline_desc.post_upscale_rkvc_model_path`。
  - CMake `RKVC_ENABLE_RKNN`（默认 ON）；未找到 `librknnrt` 时降级构建（无 NPU SR）。

- **FFmpeg 工具层**（`lib/ffmpeg_util.c`）
  - 统一日志回调（`[rkvc:ffmpeg]` 前缀）、`rkvc_now_us()`、`rkvc_dict_parse_opts()`、`rkvc_codec_open2()` 等；`rkvc_init()` 自动调用 `rkvc_ffmpeg_utils_init()`。

- **Bench 套件增强**
  - `bench/config.json`、`bench/demo_videos.json`：路径、clip、校准表、`svt.superres` 开关。
  - `bench/tools/comparison_demo_rkvc.py`：可配置码率/标签/字体，批量输出对比演示 MP4。
  - `bench/tools/config.py`、`bench/tools/bitrate.py`：配置加载与码率计算辅助。
  - SVT-AV1 + superres 实验路线（`svt-av1+superres`）；默认关闭，需 `paths.superres_decode_ffmpeg`（libaom 软解）。
  - post-upscale 路线支持 `rkvc_sr` 算法名（`{codec}+up{N}x-rkvc_sr`）。
  - RD/性能图：`plot_rd_curve.py` 重构（codec 族配色、RGA 均值带、superres 虚线）；`plot_perf.py` 同步。
  - `.gitattributes` + LFS：`docs/images/bench/rd_curve_e2e.png`、`perf_e2e.png` 纳入文档。

- **测试夹具**（`tests/test_fixtures.c/h`）
  - 自生成 NV12 图案与 H.264 MP4（Session 编码），硬件测试无需 git 内嵌媒体。
  - 硬件用例扩展：`test_session_transcode_balanced`、`test_session_transcode_quality`、`test_session_encode_decode_upscale_3x`（3× 下采样编码 + bilinear 还原）。

- **脚本**
  - `scripts/install-librga.sh`：从 airockchip/librga 安装头文件、动态库与 pkg-config（CI 三 job 统一调用）。
  - `scripts/make-comparison-demo.sh`：对比演示视频入口。

### 变更

- 版本号升至 **0.2.1**（`CMakeLists.txt` `project(VERSION)` 为唯一来源）。
- **SVT-AV1 码控**：文件/转码场景下 `RKVC_RC_CBR` 映射为 SVT `VBR`（random-access 下 CBR 初始化失败）。
- **管线字段**：`rkvc_pipeline_desc` 新增 `svt_lp`、`svt_rtc`、`codec_opts`；CLI `rkvc_encode` / `rkvc_transcode` 支持 `--svt-lp`、`--svt-rtc`。
- **端口队列**：`lib/port.c` 环形缓冲改为 FFmpeg `AVFifo`，push 失败路径补全 NOMEM/INTERNAL。
- **MPP 解码器**：`codec_opts` 经 `rkvc_dict_parse_opts` 传入 `avcodec_open2`。
- **可移植包**：`package-portable.sh` 打包 `rkvc_session_upscale`、`rkvc_yuv_upscale`。
- **`scripts/test-portable.sh`**：`rkvc_bench` 校验 REALTIME/BALANCED/QUALITY 三策略 fps（拒绝 `-1.0 fps`）；新增 `rkvc_session_upscale` 2× 后处理上采样项；**92 → 99 项**。
- **CI**：`package` job 增加 portable 构建与 `test-portable.sh`；补充 `gcc`、`pkg-config` 依赖。
- **测试结构**：移除 `tests/test_frame.c`（逻辑并入 fixtures / 其他套件）；`CMakeLists.txt` 链接 `test_fixtures.c`。
- 文档：`docs/testing.md` 硬件矩阵与 99 项 portable 说明；`docs/architecture.md` 补充 `rkvc_sr` 与 YUV-native 模型规格引用；`docs/packaging.md`、`README.md` 同步 bench 图与版本号。

### 修复

- **SVT-AV1 CBR 打开失败**：random-access 文件编码时 CBR 模式导致 SVT 初始化错误，改为 VBR 对齐 bench/转码实际行为。

### 测试

- 硬件集成：7 个独立 CTest 子用例（三策略转码 + 3× 上采样），`RKVC_RUN_HARDWARE_TESTS=1` 时夹具自生成。
- `test_post_upscale.c`：`rkvc_sr` 算法名解析回归。
- **可移植包**：`rkvc-0.2.1-linux-aarch64-portable.tar.gz` 解压后 `./test.sh` **99 项 / 0 失败**。

### 已知限制

- **SVT-AV1 + superres（`svt-av1+superres`）**：`av1_rkmpp` 硬解 superres 码流时 `hwdownload` stride 不一致会崩溃；实验路线默认关闭，启用时需 libaom 软解（慢）。
- **`rkvc_sr`**：现网模型 RGB 域训练，推理含 NV12↔RGB CSC 开销；YUV-native 模型见 `docs/sr-model-yuv-spec.md`。

## [0.2.0] - 2026-06-30

### 发布重点

rkvc **0.2.0**（v2 API）是面向 RK3588 的破坏性大版本：以 **Session + Pipeline + Codec Router** 取代 v1 的 `encoder` / `decoder` / `stream` / `frame` / `scale` API，并首次打通 **H.264 / HEVC / AV1** 三条编解码路线与 **下采样编码 + RGA 后处理上采样** 评估管线。

- **策略路由**：`REALTIME` → H.264 RKMPP；`BALANCED` → HEVC RKMPP（1080p@≥50fps 自动降级 H.264）；`QUALITY` → SVT-AV1 编码 + `av1_rkmpp` 硬解。
- **后处理上采样**：`enc_scale_denom` + `post_upscale_algo` 贯穿 Session 管线与 bench，模拟「低分辨率编码 → 硬解 DMABUF → RGA 上采样还原」产品路径。
- **可移植包**：`rkvc-0.2.0-linux-aarch64-portable.tar.gz`（约 4.5 MB），自包含 `libSvtAv1Enc.so.4`、ffmpeg-rockchip（含 AV1 硬解）与 rockchip-mpp；`test.sh` **92 项全过**。
- **RD 基准套件**：`bench/` 支持 H.264 / HEVC / SVT-AV1 / rkvc 三档策略 / post-upscale 端到端码率-画质与性能对比。

### 破坏性变更

- **公共 API 全面替换**
  - 删除：`encoder.h`、`decoder.h`、`stream.h`、`frame.h`、`scale.h` 及对应 `lib/*.c` 实现。
  - 新增：`buffer.h`（DMA-BUF 统一缓冲）、`pipeline.h`（管线模板与 `enc_scale_denom` / `post_upscale_algo`）、`policy.h`（Codec Router）、`port.h`（命名端口）、`session.h`（会话生命周期与分阶段计时统计）。
  - 核心入口：`rkvc_session_create()` → `rkvc_session_run_file()` 或 `rkvc_session_port()`；管线通过 `rkvc_pipeline_from_template()` 配置。
- **CLI 行为变更**
  - `rkvc_encode`：仅接受原始 NV12 文件（`-i raw.nv12`），移除 `--testsrc` / `--stdin` / `--stdout`；输出默认为 MP4。
  - `rkvc_decode`：`-i` 容器/码流，`-o` 原始 NV12；移除管道模式。
  - `rkvc_info --json`：字段改为 `h264_enc`、`hevc_enc`、`av1_enc`、`h264_dec`、`hevc_dec`、`av1_dec`。
  - 新增 `rkvc_transcode`：`-p realtime|balanced|quality` 策略转码。
- **包名与版本**：`rkvc-0.1.x-linux-aarch64-portable` → `rkvc-0.2.0-linux-aarch64-portable`。

### 新增

- **Codec Router 与节点图**
  - `lib/router.c`：按 `rkvc_policy`、分辨率、帧率解析 `rkvc_route_plan`。
  - 节点：`node_demux` / `node_mux` / `node_mpp_dec` / `node_mpp_enc` / `node_svt_enc` / `node_rga` / `node_dma_to_host` / `node_post_upscale`。
  - 模板：`FILE_ENCODE`、`FILE_DECODE`、`FILE_TRANSCODE`、`AV1_STORAGE`、`LIVE_CAPTURE`（占位）。

- **SVT-AV1 与 AV1 硬解**
  - 子模块 `third_party/SVT-AV1/`，`scripts/build-svt.sh` 构建 `libSvtAv1Enc.so.4`。
  - `scripts/rebuild-ffmpeg-rkmpp.sh` 启用 `h264_rkmpp` / `hevc_rkmpp` / `av1_rkmpp`。

- **RGA 上采样 API 与后处理管线**
  - 公共 API：`rkvc_upscale_yuv420p()`、`rkvc_upscale_nv12()`、`rkvc_upscale_ctx_*()`；算法 `nearest` / `bilinear` / `bicubic`。
  - Session 字段：`enc_scale_denom`（编码前 1/N 下采样）、`post_upscale_algo`（解码后 RGA 还原）；`width`/`height` 始终为显示分辨率。
  - `node_post_upscale`：RKMPP 硬解 DMABUF → RGA `importbuffer` → `imresize` → 主机 NV12。
  - CLI 工具：`rkvc_yuv_upscale`（YUV420p 批处理）、`rkvc_session_upscale`（Session 硬解 + RGA，bench 与产品同路径）。

- **RD 基准测试（bench/）**
  - `scripts/run-bench.sh`：端到端 PSNR/SSIM/码率/fps 采样，`plot_rd_curve.py` / `plot_perf.py` 绘图。
  - 默认路线：`h264`、`h265`、`svt-av1`、`rkvc-v2`（三档策略展开）、`post-upscale`（下采样编码 + Session 解码上采样）。
  - 可配置 `ENC_SCALE_DENOM`、`UPSCALE_ALGOS`、`RUN_CODECS`；支持 SVT-AV1 superres 实验路线（搁置，硬解 stride 不一致）。

- **构建与打包**
  - `scripts/build-common.sh`：统一编译并行度（默认 4）。
  - `scripts/portable-test-helpers.sh`：可移植包 NV12 生成与编码辅助。
  - `scripts/package-portable.sh`：打包 `rkvc_transcode`、`rkvc_session_upscale`、`libSvtAv1Enc` 并设置 RPATH。

### 变更

- 版本号升至 **0.2.0**（`CMakeLists.txt` `project(VERSION)` 为唯一来源）。
- `scripts/test-portable.sh`：适配 v2 头文件、JSON 字段、Session 编解码/转码、`rkvc_bench` 三策略短测；共 **92 项**。
- `scripts/network-e2e-test.sh`：v2 冒烟（码流生成 + `stream_device_pair` 占位）；完整 UDP/RTP 回环待 `LIVE_CAPTURE` 接入。
- `examples/stream_device_pair.c`：v2 占位，提示 LiveCapture/V4L2 待接。
- 全部示例改写为 Session API；`docs/architecture.md`、`docs/migration.md` 同步 v2 架构与上采样管线说明。

### 测试

- CMocka / CTest 全面改写为 v2 Session / Router / Buffer / post-upscale 测试；硬件测试通过 `RKVC_RUN_HARDWARE_TESTS=1` 串行执行。
- 新增 `test_post_upscale.c`、`test_scale` 中 `rkvc_post_upscale_buffer` 与 `enc_scale_denom` 硬件回归。
- `scripts/test-rga.sh`：1080p↔360p、padding 源、post_upscale soak 门禁。
- **可移植包**：`rkvc-0.2.0-linux-aarch64-portable.tar.gz` 解压后 `./test.sh` **92 项 / 0 失败**；覆盖 RPATH 自包含、编解码转码、pkg-config、负向包结构检测。

### 迁移提示（v0.1.x → v0.2.0）

```c
// v0.1.x
rkvc_encoder *enc = rkvc_encoder_open(&cfg);
rkvc_encoder_send_frame(enc, frame);
rkvc_encoder_drain(enc);

// v0.2.0
rkvc_pipeline_desc d;
rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
d.input_path = "raw.nv12";
d.output_path = "out.mp4";
d.policy = RKVC_POLICY_REALTIME;
rkvc_session *s;
rkvc_session_create(&d, &s);
rkvc_session_run_file(s);
rkvc_session_destroy(s);
```

```bash
# v0.1.x
rkvc_encode --testsrc -o test.h265 -s 1920x1080 -n 100

# v0.2.0
./examples/bin/example_encode_file -o test.mp4 -s 1920x1080 -n 100
# 或
rkvc_encode -i raw.nv12 -o out.mp4 -s 1920x1080 -p realtime

# 下采样编码 + 后处理上采样（评估 NN 占位）
rkvc_session_upscale -i stream.mp4 -o out.nv12 \
  --width 1920 --height 1080 --enc-scale-denom 2 --post-upscale bilinear
```

## [0.1.6] - 2026-06-23

### 发布重点

- 修复 `rkvc_decoder_config.output_format` 配置失效 bug：配置 YUV420P / NV16 / P010 等非 NV12 格式后，`rkvc_frame_get_info` 返回的格式始终为 NV12。
- 重新构建 `ffmpeg-rockchip` 启用 `libswscale`，解码器对硬件无法直接输出的格式通过软件像素格式转换实现，保证交付帧格式严格等于配置值。
- 新增 `decode_formats` 示例程序，用同一 H.265 流分别以 NV12 / YUV420P / NV16 / P010 解码并逐帧打印实际格式，作为本次修复的可运行验证。
- 补齐此前仅 fake-context / 错误路径覆盖的 API 真实硬件功能性测试。

### 修复

- **`rkvc_decoder_receive_frame` 输出格式失效**
  - 根因：RKMPP 硬件帧池对输出格式的支持受输入码流类型严格约束（8-bit HEVC 仅能直接输出 NV12，10-bit 仅能输出 NV15 等）。原实现未显式指定下载目标格式，FFmpeg 回退到硬件帧池默认 sw_format（NV12）；且工程构建 `ffmpeg-rockchip` 时用了 `--disable-swscale`，软件格式转换链路缺失。两者叠加导致非 NV12 配置静默失效。
  - 修复：解码器先尝试让硬件直接输出请求格式；若硬件帧池不支持（transfer 失败或静默回退），下载到硬件默认格式后调用 libswscale 软转换为请求格式。最终交付帧格式保证等于 `cfg.output_format`。
  - 兼容性：NV12 走硬件直出无额外开销；非 NV12 格式引入一次软件转换的 CPU 开销，属合理代价。调用方无需修改代码。
  - 影响：解码器、可移植包库均需重新链接 `libswscale.so.7`。

### 新增

- **`examples/decode_formats.c` 示例**
  - 编码一段 320×240 测试 H.265 流，再分别以 NV12 / YUV420P / NV16 / P010 作为 `output_format` 解码，逐帧打印 `info.format` 并与配置比对。
  - 真机运行四种格式全部 `✓`，对应 `examples/decode_formats.c`；已纳入 `CMakeLists.txt` 示例列表与可移植包 `examples/bin`、`examples/src`。

### 变更

- `third_party/ffmpeg-rockchip` 重新配置：`--disable-swscale` → `--enable-swscale`，`libswscale.a` / `libswscale.so.7` 已生成。
- `CMakeLists.txt`：`FFMPEG_LIBS` 加入 `swscale`，`AVCODEC_LIB_DIRS` 加入 `libswscale` 路径；示例列表加入 `decode_formats`。
- `lib/internal.h`：新增 `#include <libswscale/swscale.h>`。
- `scripts/package-portable.sh`：可移植包库复制列表与 RPATH 循环加入 `libswscale`；自包含校验列表加入 `libswscale`。
- `.github/workflows/ci.yml`：test 与 package 两个 job 的 ffmpeg 构建参数改为 `--enable-swscale`。
- 发布文档同步：`docs/release/README.md` 示例列表加入 `decode_formats`；`docs/release/EXAMPLES.md` 新增 decode_formats 章节；`docs/release/USAGE.md` 修正版本示例输出 `0.1.4 → 0.1.6` 并补充 decode_formats 用法。

### 测试

- **新增回归测试**
  - `test_hardware.c::test_decoder_output_format_is_respected`：编码短 8-bit HEVC 流，以 NV12 / YUV420P / NV16 / P010 分别解码，校验每一帧 `info.format` 与配置完全一致。所有格式现在都必须成功（NV12 走硬件，其余走 sws_scale）。
  - `test_internal.c::test_frame_wrap_preserves_non_nv12_formats`：无硬件依赖，验证 `rkvc_frame_wrap_avframe` 对 NV12/YUV420P/NV16/P010 都能正确翻译 `AVFrame->format` → `rkvc_frame_info.format`。
- **补齐此前仅 fake-context / 错误路径覆盖的 API 真实硬件功能性测试**
  - `test_types.c`：新增 `rkvc_init` / `rkvc_deinit` 的幂等性、未配对安全、init↔deinit 循环测试（此前完全无覆盖）。
  - `test_hardware.c::test_encoder_no_file_mode_and_send_buffer`：编码器无文件模式 + `send_buffer` 零拷贝接口 + `drain` + `timebase` + `get_config` 真实值验证（此前仅 NULL 错误路径 / fake-context）。
  - `test_hardware.c::test_decoder_callback_mode_and_drain`：解码器文件模式下 `get_video_info` / `get_duration` 真实值验证 + `drain` 真实硬件路径（此前仅 fake-context）。
- CMocka 用例总数从 68 增至 80，全部通过。
- **可移植包完整测试**：重建 `rkvc-0.1.6-linux-aarch64-portable.tar.gz`（含新增 `example_decode_formats` 与更新后的发布文档），`test.sh` 自测通过 84 项 / 0 失败；全新目录解压后自测同样 84 项全过；`network-e2e-test.sh` UDP 与 RTP 双模式本机回环均通过；`example_decode_formats` 真机运行四种输出格式全部 `✓`。

## [0.1.5] - 2026-06-18

### 发布重点

- 修复 `rkvc_frame_scale()` 在 1920×1080 NV12 帧底部产生 16 行纯绿色带的硬件错位 bug，根因是 ffmpeg `av_frame_get_buffer()` 与 RGA `wrapbuffer_virtualaddr_t()` 对 UV 平面偏移的假设不一致。
- 凡是高度不是 32 倍数的帧（典型如 1080p）经过 `rkvc_frame_scale` 都会受影响；修复后帧底字节级与输入一致，PSNR 从 24.27 dB 提升至 ∞（同分辨率缩放），全部图像内容完整保留。
- 新增 3 个回归测试钉住该问题，反向验证：移除修复后 3 个用例立即失败并精确定位错误位置。

### 修复

- **`rkvc_frame_scale` 帧底绿色带 (UV 错位)**
  - 根因：ffmpeg `av_frame_get_buffer(0)` 会按 32 行高度对齐 + 每平面 16 字节 padding，对 1920×1080 NV12 实际产生 `data[1] = data[0] + linesize[0]*1088 + 32` 的布局（Y 与 UV 之间存在 15392 字节 gap）。RGA `wrapbuffer_virtualaddr_t()` 用单一基址 + `wstride*hstride` 推算 UV 地址，没有字段表达这个 gap，导致 RGA 把 UV 写到错误偏移；最终在帧底 16 luma 行处 UV=(0,0) 显示为纯绿色。
  - 修复：`rkvc_frame_alloc()` 不再调用 `av_frame_get_buffer()`，改为 `av_image_get_buffer_size + av_buffer_alloc + av_image_fill_arrays(align=1)` 自行分配严格连续的像素缓冲，让 RGA 的偏移算式与实际内存严格一致。受影响调用方包括 `examples/transcode.c`、`lib/stream.c` 自动缩放路径。
  - 兼容性：对外仍是连续布局的 `AVFrame`，`rkvc_frame_get_data()` 行为不变；不需要修改任何调用者代码。
  - 性能：fast path 零损失（实测 7.23 ms / 1080p NV12 RGA scale 不变）；如果调用方传入的源帧带 ffmpeg padding（例如直接来自 `av_hwframe_transfer_data`），`rkvc_frame_scale` 会先做一次 `av_image_copy`（约 0.4 ms / 1080p）再喂 RGA，保证正确性。

### 测试

- **回归测试**
  - 新增 `test_frame_alloc_contiguous_layout`：纯 CPU 校验 `rkvc_frame_alloc` 输出的 NV12/YUV420P 帧 `data[1] == data[0] + linesize[0]*H`，覆盖 1080p / 480p / 720p / 1440p。
  - 新增 `test_scale_identity_byte_exact_nv12_1080p`：1920×1080 NV12 经 `rkvc_frame_scale` 同分辨率缩放后必须与输入逐字节一致；用线性同余生成的随机 UV 内容，避免常数色块掩盖错位；额外检查帧底 16 行不存在 UV=(0,0) 全零行。
  - 新增 `test_scale_identity_byte_exact_yuv420p_1080p`：同上但用三平面 I420，覆盖 V 平面同样的偏移问题。
  - 反向验证：临时撤销修复，仅这 3 个新用例失败并报告具体错误位置（如 `[UV] mismatch row 531 col 1888: got=0 ref=41`），其余 11 个用例继续通过。

### 内部

- 新增 `rkvc_avframe_alloc_contiguous(AVFrame*)` 内部 helper，封装"严格连续无 padding"的 ffmpeg 帧缓冲分配。
- 新增 `frame_is_contiguous_for_rga(AVFrame*)` 检查，识别外部传入帧是否安全直接喂 RGA，覆盖 NV12/NV21/NV16/YUV420P/P010。

## [0.1.4] - 2026-06-05

### 发布重点

- 交付包从“能运行”升级为“可验证”：portable 包内新增一键自测与本机网络端到端回环，覆盖文件、依赖、RPATH、CLI、编解码、pkg-config、负向包结构和 UDP 网络链路。
- 硬件启动前增加设备权限门控，权限不足时返回明确错误，避免落入 RKMPP/FFmpeg 初始化后的不稳定路径。
- portable 包改为随包携带自建 rockchip-mpp 动态库，并通过 RPATH/RPATH-link 和自测防止解析到系统旧版 MPP。

### 新增

- **设备权限与输入格式错误码**
  - 新增 `RKVC_ERR_PERMISSION`，用于区分设备节点权限不足与一般硬件初始化失败；`rkvc_err_str()` 返回 `device permission denied`。
  - 新增 `RKVC_ERR_FORMAT` 和 `rkvc_probe_input_format()`，用于识别 H.264/H.265 Annex-B 与常见容器 magic，防止压缩码流被误当作原始 NV12 输入。

- **硬件权限前置检测**
  - RKMPP 编码/解码打开路径在 `avcodec_open2()` 前检查 `/dev/mpp_service` 和 MPP 实际优先使用的 DMA heap 子节点。
  - DMA heap 预检按 rockchip-mpp 默认选择顺序检查 `system-uncached`、`system`、`system-uncached-dma32`、`system-dma32`，避免目录可读但子节点不可读时进入 MPP 崩溃路径。
  - `rkvc_query_caps()` 现在按当前用户权限报告 MPP、DMA heap 和 RGA 能力。

- **可移植包一键自测与网络回环**
  - portable 包根目录新增 `test.sh`，可在包目录内直接执行完整验证。
  - 新增 `network-e2e-test.sh`，自动生成测试 H.265 码流，通过 `127.0.0.1` UDP/RTP 回环模拟网络传输，再由接收端解码并校验发送/接收帧数。
  - `scripts/test-portable.sh` 支持无参数包内运行，并把本机 UDP 网络端到端编解码回环纳入默认自测。

- **SDL2 可视化质量预览示例**
  - 新增 `examples/visual_compare.c`，并排展示输入原始帧与重新编码解码后的重建帧。
  - 底部实时显示码率、压缩比、端到端延迟、稳定性和 Y/U/V/加权 PSNR。
  - CMake 新增 `RKVC_BUILD_GUI_EXAMPLES` 选项；未检测到 SDL2 时自动跳过 GUI 示例，不增加核心库硬依赖。

### 修复

- **portable 包 MPP 运行库串入系统旧版本**
  - `librkvc.so` 现在保留对 `librockchip_mpp.so.1` 的直接运行时依赖，避免工具链接或运行时解析到系统旧版 MPP。
  - CMake 为库、工具、示例和测试目标加入本地依赖目录与 `rpath-link`，默认构建不再需要手动设置 `LD_LIBRARY_PATH` 才能解析 MPP 符号。
  - 打包校验新增“关键库必须解析到包内 `lib/`”检查，防止系统库误串入。

- **CLI 压缩输入误用**
  - `rkvc_encode -i` 现在会探测输入文件头；发现 H.265/H.264/MP4/MKV 等压缩视频时直接报错并提示改用解码或转码路径。
  - `rkvc_bench` 子测试失败时返回非 0，部署脚本和包自测可以可靠捕获失败。

### 变更

- **打包脚本更新** (`scripts/package-portable.sh`)
  - 自动从 `third_party/mpp` 构建并安装 rockchip-mpp，再用该 MPP 构建 ffmpeg-rockchip 和 rkvc。
  - 可移植包携带 `librockchip_mpp` / `librockchip_vpu` 动态库，并为这些库设置 `$ORIGIN` RPATH。
  - 打包时复制示例程序源码/二进制、发布文档、`test.sh` 和 `network-e2e-test.sh`。
  - 目标板前置依赖说明移除 `librockchip-mpp1`，仅保留系统 DRM/RGA 相关依赖；当前发布包大小约 2.5 MB。

- **构建与测试矩阵**
  - CMake 新增 `RKVC_BUILD_GUI_EXAMPLES`，SDL2 不存在时跳过 GUI 示例，不影响核心库、CLI 或其他示例。
  - 新增 `full-tests` preset，在单元测试基础上构建 CLI 工具并运行脚本回归。
  - 测试目标统一带上包内依赖路径，减少裸环境下 `LD_LIBRARY_PATH` 对测试结果的影响。

- **发布文档同步**
  - release README/USAGE/EXAMPLES 增加本机网络端到端测试命令。
  - `stream_device_pair` 文档参数更新为当前 CLI 的 `-c`、`--dst-ip`、`--dst-port`、`--bind-port`。
  - packaging/testing/delivery 文档同步 portable 包目录结构、MPP 动态库携带方式、RPATH 行为和当前自测覆盖范围。

### 测试

- 增加 `test_permissions`，通过 fake `/dev` 覆盖 `/dev/mpp_service`、MPP 默认 DMA heap、DRM fallback 和 `rkvc_query_caps()` 权限门控回归。
- 增加 `full-tests` CMake preset，并新增 `test_cli_args`、`test_bench_permission_failure` 两个 CTest 脚本目标。
- 增加流式 API 边界测试，覆盖统计值、重复 finish、finish 后 pull、`buffer_size` 上限等路径。
- 增加输入格式探测回归，覆盖 H.264/H.265 Annex-B、MP4 magic，以及编码器拒绝压缩码流误作为原始 NV12 输入的 SDK/CLI 路径。
- portable 包 `test.sh` 增加 pkg-config 最小程序编译运行、CLI 参数错误、不可执行工具、缺失/串入系统 MPP 库、绝对 RPATH 注入等负向测试。
- `network-e2e-test.sh` 已验证 UDP 与 RTP 本机回环；portable 包 `test.sh` 默认执行 UDP 端到端回环。
- 当前 `tests` preset 为 8 个 CTest 目标 / 68 个 CMocka 用例；`full-tests` 为 10 个 CTest 目标；portable 包自测当前 81 项全部通过。

## [0.1.3] - 2026-05-19

### 变更

- **发布文档优化**
  - 新增独立发布文档目录 `docs/release/`，包含 README、USAGE、EXAMPLES、DEVELOPMENT 四份用户文档
  - 移除发布包中的 LICENSE 文件
  - 发布文档采用通用技术描述（"硬件编解码"），移除具体实现细节（RKMPP/FFmpeg/RGA/H.265），保护知识产权
  - 命令示例和 API 参数保留真实文件扩展名（.h265）和参数值，确保事实准确性
  - 移除设备权限配置命令，改为引导用户联系技术支持
- **打包脚本更新** (`scripts/package-portable.sh`)
  - 自动复制 `docs/release/` 目录内容到发布包根目录
  - 不再复制 LICENSE 文件
- 版本号提升至 0.1.3

## [0.1.2] - 2026-05-18

### 新增

- **真实 UDP 网络传输** (`examples/stream_device_pair.c`)
  - 将原来的三种模拟模式（`ring`/`shm`/`rtp` 均为进程内内存模拟）全部替换为真实网络传输：
    - `udp` — 原始编码帧 over UDP Socket（16B 头: frag_id+frag_total+frame_len+pts, 大帧自动分片最多 16 片）
    - `rtp` — RTP 封包 over UDP Socket（H.265 NAL 分片 ≤1400B, Marker 位标记帧边界, SSRC）
  - 支持 `-r send|recv|both` 单角色/双角色部署模式，真正实现两台物理 RK3588 之间的流式传输
  - 一个板卡解码文件 + 重编码 → UDP 发送，另一个板卡 UDP 接收 + 实时解码
  - 提取通用 UDP Socket 辅助层，`udp` 和 `rtp` 两通道共享
- **UDP 大帧分片与重组**: IDR 帧可达 80–120 KB，超过单 UDP 数据报 65507 字节上限，`udp` 通道新增分片协议（`frag_id`/`frag_total`/`frag_mask` 位图去重），接收端自动组装交付
- **API 文档 UDP 传输须知**: `docs/api.md` 新增 warning 块，说明编码帧可能超过 UDP 数据报大小，给出分片协议头字段参考

### 修复

- **转码示例降分辨率失败**: `examples/transcode.c` 直接将解码帧送入不同分辨率的编码器导致 RKMPP 报 `invalid parameter`，修复为当分辨率不匹配时先用 `rkvc_frame_scale()` RGA 硬件缩放再送入编码器。
- **打包脚本符号链接**: `scripts/package-portable.sh` 修复动态库符号链接过多问题（如 `libavcodec.so.60.31` 中间层级），简化为标准 `libfoo.so → libfoo.so.X → libfoo.so.X.Y.Z`，`librkvc` 链接统一由循环生成。
- **测试脚本版本兼容**: `scripts/test-portable.sh` ffmpeg 库检查改为通配符匹配，不再硬编码具体版本号。

### 变更

- **打包脚本优化** (`scripts/package-portable.sh`)
  - 示例程序二进制（`example_*`）和源码（`examples/*.c`）自动打包到 `examples/bin/` 和 `examples/src/`
  - 示例二进制 RPATH 设置为 `$ORIGIN/../../lib`
- `rtp` 接收端设置 1 秒 `SO_RCVTIMEO` 超时，`finished` 标志改为 `__sync_synchronize` 保证跨线程可见

## [0.1.1] - 2026-05-17

### 新增

- **RGA 硬件缩放 API** (`rkvc_frame_scale`)
  - 基于 Rockchip RGA 2D 加速器，零 CPU 占用完成 NV12/YUV420P/NV16/P010 帧的缩放和格式转换
  - 支持 upscaling、downscaling、同分辨率复制
  - 自动保留源帧 PTS
  - 新增头文件 `include/rkvc/scale.h`，公共入口 `rkvc.h` 已自动包含

- **流式编码自动缩放**
  - `rkvc_stream_push()` 编码路径新增自动 RGA 缩放：当输入帧尺寸或像素格式与流配置不匹配时，内部自动调用 RGA 缩放/转换后再送入编码器
  - 无需调用方手动处理分辨率适配

- **示例程序**
  - `stream_transcode` — 流式转码管线示例（解码 → 自动缩放 → 编码），支持 `-s WxH` 参数指定输出分辨率
  - `stream_device_pair` — 双设备流式传输模拟示例，支持三种通道模式：
    - `ring` — 环形缓冲区（模拟 UDP 局域网）
    - `shm` — 共享内存 IPC（模拟 RTOS 零拷贝消息队列）
    - `rtp` — RTP/PS 封包（模拟 GB/T 28181 国标传输）

- **测试**
  - 新增 `test_scale` 测试套件（11 项），覆盖：
    - RGA 可用性检测
    - 参数校验（NULL 指针、零尺寸）
    - 硬件缩放（下采样、上采样、同分辨率、PTS 保留、1080p→720p 真实场景）
    - 流自动缩放集成测试

### 变更

- 构建系统新增链接 `rga`、`rt` 库
- `CMakeLists.txt` 示例目标列表新增 `stream_transcode`、`stream_device_pair`
- 测试目标列表新增 `test_scale`
- `.gitignore` 新增 `*.mp4`、`*.h264`、`*.h265` 规则，避免测试媒体文件入库

### 修复

- **P010 格式映射**：`rkvc_frame_scale` 的 P010 映射从错误的 8-bit NV12 (`RK_FORMAT_YCbCr_420_SP`) 修正为正确的 10-bit 格式 (`RK_FORMAT_YCbCr_420_SP_10B`)
- **流自动缩放目标格式**：`rkvc_stream_push()` 的缩放配置现在显式指定 `dst_format = s->config.input_format`，确保格式转换与编码器期望一致
- **示例错误处理**：`stream_device_pair` 发送端现在检查 `rkvc_stream_push` 返回值，失败时报错并终止

### CI

- **librga 依赖修复**：`librga-dev` 不在 Ubuntu 24.04 标准仓库中，改为从 `airockchip/librga` 上游仓库 clone 预编译头文件和 aarch64 动态库，修正头文件安装路径（`include/*.h` → `/usr/local/include/rga/`）
- **GitHub Actions 版本升级**：`actions/checkout` v4 → v6，`actions/upload-artifact` v4 → v7，解决 Node.js 20 废弃警告

### 打包

- 修复 `scripts/package-portable.sh`：
  - 自动检测 CMake 生成器（Ninja/Unix Makefiles），不再硬编码
  - `librkvc` 版本号改为通配符匹配，避免每次发版修改脚本
  - ffmpeg 库过滤为仅打包 libavcodec/libavformat/libavutil（去除 libavdevice/libavfilter/libpostproc）
  - 为 librkvc.so 和 ffmpeg 库设置 `$ORIGIN` RPATH，确保自包含
  - 使用独立 `build-portable/` 目录，避免与已有 `build/` 冲突
  - 验证步骤修复为 process substitution，正确报告未解析依赖

## [0.1.0] - 2026-05-14

### 新增

- 初始版本发布
- H.265 (HEVC) RKMPP 硬件编码器/解码器
- 流式 API（编码流/解码流，内部环形缓冲区，支持异步 push/pull）
- 文件模式（编解码器自动 mux/demux）
- CLI 工具（rkvc_encode、rkvc_decode、rkvc_info、rkvc_bench）
- 示例程序（encode_file、decode_file、stream_encode、stream_decode、transcode、latency_test、psnr_test）
- 运行时能力查询（`rkvc_query_caps`）
- 完整单元测试套件（CMocka）
- ASan/UBSan/覆盖率构建预设
- 可移植二进制包打包脚本
- CMake 构建系统，支持 pkg-config 和 CMake config 安装
