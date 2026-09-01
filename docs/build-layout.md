# 构建目录

| 路径             | 内容                   |
| ---------------- | ---------------------- |
| .build/release/  | 默认 Release 构建      |
| .build/debug/    | Debug 构建             |
| .build/tests/    | 单元测试               |
| .build/deps/     | 本机构建依赖前缀       |
| .build/portable/ | 交叉构建、暂存树和归档 |

发布包只从 CMake install tree 生成。
