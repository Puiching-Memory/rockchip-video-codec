# 构建目录

`.build/` 下只有三个预设目录（见 `CMakePresets.json`）：

| 路径 | 内容 |
| ---- | ---- |
| `.build/release/` | `default` 预设：Release 构建（`rkvc` + `rkvc_*.so`） |
| `.build/debug/` | `debug` 预设：Debug 构建 |
| `.build/tests/` | `tests` 预设：Debug + `RKVC_CORE_BUILD_TESTS=ON`，`ctest` 在此跑 |

本仓没有 install 规则、不产安装树：板端部署直接复制构建树产物
（CLI + 所需插件 `.so` + `.rkmdl`），用 `--backend-dir/--model-dir`
指向它们。旧 `portable/`、`deps/`、`check`、`docs` 目标与
`RKVC_ENABLE_*` 系列选项已随旧 C 树删除。
