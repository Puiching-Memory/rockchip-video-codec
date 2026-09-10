# 工具目录

`tools/` 是项目所有开发、构建、发布和性能工具的统一入口：

- `check-symbols.sh`：新 C++ 产物的 GLIBC 上限、NEEDED 与导出面审计。
- `check-format.sh`：C/C++ 源码格式校验（`--fix` 就地修复）。
- `bench/`：Rockchip 实机性能基准。
- `mlvc/`、`sr/`：模型导出及转换工具。

仓库不再维护平行的 `scripts/` 目录。
