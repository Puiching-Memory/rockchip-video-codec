# ffmpeg-rockchip downstream patches

rkvc 0.4 uses the native MPP backend for its main media graph, but keeps these
patches for applications that also build `third_party/ffmpeg-rockchip`.

`0001-rkmppenc-roi-runtime-rc.patch` maps FFmpeg
`AV_FRAME_DATA_REGIONS_OF_INTEREST` to MPP `KEY_ROI_DATA`, supports the
`rkvc_roi_force_intra` frame metadata key, and reapplies bitrate/GOP changes at
runtime. It is pinned to the ffmpeg-rockchip submodule commit recorded by this
repository.

Validate without changing the submodule:

```sh
cmake -DGIT_EXECUTABLE="$(command -v git)" \
      -DRKVC_SOURCE_DIR="$PWD" \
      -P cmake/CheckFfmpegPatches.cmake
```

Build scripts should apply patches only in a temporary source worktree, or
reverse them in an EXIT trap. Never commit a dirty ffmpeg-rockchip gitlink.
