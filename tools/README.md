# 工具目录

`tools/` 是项目所有开发、构建、发布和性能工具的统一入口：

- `build-common.sh`：依赖构建使用的 shell 公共函数。
- `check-exported-symbols.sh`：公共 ABI 导出检查。
- `rkvc-build` / `rkvc_build/`：可移植包构建、验证和归档。
- `bench/`：Rockchip 实机性能基准。
- `mlvc/`、`sr/`：模型导出及转换工具。

仓库不再维护平行的 `scripts/` 目录。
