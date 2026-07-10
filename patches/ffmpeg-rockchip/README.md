# ffmpeg-rockchip 补丁

父仓库维护对 `third_party/ffmpeg-rockchip` 的增量补丁；**不要**直接改子模块并提交 dirty gitlink。

## 用法

`./scripts/rebuild-ffmpeg-rkmpp.sh`（以及 `rebuild-ffmpeg-av1.sh` / `package-portable.sh`）
在 configure 前通过 `rkvc_apply_ffmpeg_patches` 幂等应用本目录下 `*.patch`。

## 当前补丁

| 文件 | 作用 |
|------|------|
| `0001-rkmppenc-roi-runtime-rc.patch` | `rkmppenc`：硬 ROI（`KEY_ROI_DATA` + `rkvc_roi_force_intra`）与运行时码率/GOP（`MPP_ENC_SET_CFG`） |

## 注意

- 应用后子模块工作区可能显示本地修改，属预期；提交父仓库时勿把 dirty 子模块状态一并提交。
- 需要干净源码树时：`git -C third_party/ffmpeg-rockchip checkout -- .`
- 升级 ffmpeg-rockchip 子模块 pin 后，请重新 `git apply --check` 验证补丁仍可应用。
