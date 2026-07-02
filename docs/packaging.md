# 打包与分发

## 可移植包 (推荐)

从源码构建，核心运行库随包携带，解压即用：

```bash
git submodule update --init --depth 1
git submodule update --init --depth 1 third_party/SVT-AV1

./scripts/package-portable.sh

./scripts/test-portable.sh build/portable/rkvc-0.2.1-linux-aarch64-portable
```

产物：`rkvc-0.2.1-linux-aarch64-portable.tar.gz`（约 4.5 MB）

```
rkvc-0.2.1-linux-aarch64-portable/
├── bin/
│   ├── rkvc_encode
│   ├── rkvc_decode
│   ├── rkvc_transcode
│   ├── rkvc_session_upscale   # 硬解 + 后处理上采样
│   ├── rkvc_yuv_upscale
│   ├── rkvc_info
│   └── rkvc_bench
├── lib/
│   ├── librkvc.so*
│   ├── libavcodec.so.60     # ffmpeg-rockchip (H.264/HEVC/AV1 RKMPP)
│   ├── libavformat.so.60
│   ├── libavutil.so.58
│   ├── libswscale.so.7
│   ├── libSvtAv1Enc.so.4    # v2 新增
│   ├── librockchip_mpp.so.1
│   └── ...
├── include/rkvc/            # v2 头文件
├── share/pkgconfig/rkvc.pc
├── examples/                # 示例源码与二进制
├── test.sh                  # 一键自测（99 项）
├── network-e2e-test.sh      # v2 冒烟测试
├── portable-test-helpers.sh
├── README.md / USAGE.md / DEVELOPMENT.md / EXAMPLES.md
```

### 使用

```bash
tar xzf rkvc-0.2.1-linux-aarch64-portable.tar.gz
cd rkvc-0.2.1-linux-aarch64-portable

./test.sh
./network-e2e-test.sh

./bin/rkvc_info -j
./bin/rkvc_transcode -i in.mp4 -o out.mp4 -p balanced

# 二次开发
gcc -o myapp myapp.c -Iinclude -Llib -lrkvc
LD_LIBRARY_PATH=lib ./myapp
```

包内工具已设置 RPATH，优先加载包内 `lib/`；二次开发程序可通过 `LD_LIBRARY_PATH=lib` 运行。

### 可复现性

所有二进制从源码构建：

- **rockchip-mpp**: `third_party/mpp/` 子模块
- **ffmpeg-rockchip**: `third_party/ffmpeg-rockchip/`，含 AV1 硬解
- **SVT-AV1**: `third_party/SVT-AV1/` 子模块
- **rkvc**: 项目源码

包内自测确认关键库解析到包内 `lib/`，避免串入系统旧版本。

## DEB 包

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build -j4 package
sudo dpkg -i build/packages/rkvc_0.2.1_arm64.deb
```

!!! note
    DEB 包依赖系统上的 ffmpeg-rockchip 和 librockchip-mpp。

## CPack TGZ

开发者 SDK 包（不含 ffmpeg 依赖）：

```bash
ninja -C build -j4 package
# 产物: build/packages/rkvc-0.2.1-Linux.tar.gz
```

## 打包脚本

| 脚本 | 用途 |
|------|------|
| `scripts/package-portable.sh` | 从源码构建可移植包 |
| `scripts/package-portable.sh --clean` | 清理重建 |
| `scripts/test-portable.sh <dir>` | 测试可移植包（99 项） |
| `scripts/build-svt.sh` | 构建 SVT-AV1 |
| `scripts/rebuild-ffmpeg-rkmpp.sh` | 重建 ffmpeg-rockchip |
| `<package>/test.sh` | 包内一键自测 |
| `<package>/network-e2e-test.sh` | v2 冒烟（码流生成 + stream_device_pair） |

## 发布文档模板

可移植包附带的用户文档源文件位于 `docs/release/`，打包时复制到包根目录。
