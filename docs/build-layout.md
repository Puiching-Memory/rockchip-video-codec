# 构建目录约定

所有构建产物收在隐藏目录 **`.build/`** 下，由 `CMakePresets.json` 的 `binaryDir` 定义。根目录不再并列 `build/`、`build-tests/` 等树；**不要**再手搓 `build-v2`、`build_dbg` 之类别名。

`bench/`、`bench_results/` 不在本约定内（独立工作区）。

## 目录一览

| 目录 | Preset | 用途 |
|------|--------|------|
| `.build/release/` | `default` | 日常 Release：库、CLI、示例 |
| `.build/debug/` | `debug` / `tidy` | Debug / clang-tidy |
| `.build/tests/` | `tests` | 单元测试 + fault injection |
| `.build/asan/` | `asan` | ASan + UBSan |
| `.build/full-tests/` | `full-tests` | 测试 + CLI 脚本用例 |
| `.build/coverage/` | `coverage` | gcov 覆盖率 |
| `.build/portable/` | `portable` | 可移植包**编译树** |
| `.build/dist/` | （`package-portable.sh`） | 可移植包**成品** |
| `.build/deps/` | （脚本） | MPP / SVT-AV1 / FFmpeg / librga install |

## 产物落点（勿混用）

| 产物 | 路径 |
|------|------|
| 日常二进制 / `librkvc` | `.build/release/` |
| CTest / `test_*` | `.build/tests/`（或 asan / coverage / full-tests） |
| 可移植包成品 | `.build/dist/rkvc-<ver>-linux-<arch>-portable/` |
| 依赖安装前缀 | `.build/deps/{mpp,svt-av1,ffmpeg,librga}-install/` |

同名二进制出现在多个子目录是**预期行为**（配置不同）。文档与日常开发一律用 `.build/release/`；测试用对应 preset 目录；打可移植包用 `./scripts/package-portable.sh`。

## 推荐命令

```bash
cmake --preset default && cmake --build --preset default
cmake --preset debug   && cmake --build --preset debug
cmake --preset tests   && cmake --build --preset tests

# CMake < 3.21（无 preset）时手写 -B，目录名须与上表一致：
cmake -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C .build/release -j4
```

环境变量：`RKVC_BUILD` / `RKVC_BUILD_DIR` 默认指向 `.build/release/`。

## 清理

```bash
# 只清 rkvc 本体（保留依赖）
rm -rf .build/release .build/debug .build/tests .build/asan \
       .build/full-tests .build/coverage .build/portable .build/dist

# 连依赖一起清（重建耗时长）
rm -rf .build
```

根目录若仍有旧的 `build/`、`build-*` 残留，可直接删除（已被 `.gitignore` 忽略）。
