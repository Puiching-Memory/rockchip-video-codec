#!/usr/bin/env python3
"""可复用演示视频管道：AV1 + RKVC SR 3× AI 上采样 @ 1080p。

左：1080p AV1 参考码流（fair 模式）
右：1/3 分辨率 AV1 低码率编码 → RKVC SR 3× 上采样还原 1080p
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

_BENCH_DIR = Path(__file__).resolve().parents[1]
_PROJECT_ROOT = _BENCH_DIR.parent
if str(_BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(_BENCH_DIR))

from tools.bitrate import actual_kbps  # noqa: E402
from tools.config import load_config, lookup_calibration  # noqa: E402

DEFAULT_FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
DEFAULT_FONT_COLOR = "0x66E0FF"
DEFAULT_DISPLAY_HEIGHT = 1080
DEFAULT_ENC_SCALE_DENOM = 3
DEFAULT_REFERENCE_KBPS = 1600
DEFAULT_TARGET_KBPS = 350
DEFAULT_SWEEP_PERIOD = 8.0
DEFAULT_RIGHT_LABEL = "RKVC编解码后效果"


@dataclass(frozen=True)
class VideoInfo:
    path: str
    width: int
    height: int
    fps_num: int
    fps_den: int
    duration: float
    codec: str
    bitrate_kbps: float

    @property
    def fps(self) -> float:
        return self.fps_num / self.fps_den if self.fps_den else 30.0


@dataclass(frozen=True)
class BitrateSegment:
    start: float
    end: float
    kbps: float


@dataclass(frozen=True)
class RkvcPaths:
    ffmpeg: str
    ffprobe: str
    svt_enc: str
    rkvc_build: str
    rkvc_yuv_upscale: str
    rkvc_session_upscale: str
    rkvc_sr_model: str
    mpp_lib: str
    svt_prefix: str
    ffmpeg_src: str
    bench_config: Path
    project_root: Path


def run(
    cmd: list[str],
    *,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc


def load_rkvc_paths() -> RkvcPaths:
    cfg_path = _BENCH_DIR / "config.json"
    cfg = load_config(cfg_path, _PROJECT_ROOT)
    paths = cfg["paths"]
    build = paths.get("rkvc_build") or str(_PROJECT_ROOT / "build")
    return RkvcPaths(
        ffmpeg=paths["ffmpeg"],
        ffprobe=paths["ffprobe"],
        svt_enc=paths.get("svt_enc") or "",
        rkvc_build=build,
        rkvc_yuv_upscale=str(Path(build) / "rkvc_yuv_upscale"),
        rkvc_session_upscale=str(Path(build) / "rkvc_session_upscale"),
        rkvc_sr_model=paths.get("rkvc_sr_model") or "",
        mpp_lib=paths.get("mpp_lib") or "",
        svt_prefix=paths.get("svt_prefix") or "",
        ffmpeg_src=paths.get("ffmpeg_src") or "",
        bench_config=cfg_path,
        project_root=_PROJECT_ROOT,
    )


def ld_env(paths: RkvcPaths) -> dict[str, str]:
    lib_dirs: list[str] = []
    if paths.mpp_lib:
        lib_dirs.append(paths.mpp_lib)
    if paths.ffmpeg_src:
        ff_root = Path(paths.ffmpeg_src)
        for name in (
            "libavutil",
            "libavcodec",
            "libavformat",
            "libavfilter",
            "libswscale",
            "libswresample",
            "libavdevice",
            "libpostproc",
        ):
            p = ff_root / name
            if p.is_dir():
                lib_dirs.append(str(p))
    if paths.svt_prefix:
        lib_dirs.append(str(Path(paths.svt_prefix) / "lib"))
    lib_dirs.append(paths.rkvc_build)

    env = os.environ.copy()
    prev = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = ":".join(lib_dirs + ([prev] if prev else []))
    if paths.svt_prefix:
        env["PATH"] = f"{Path(paths.svt_prefix) / 'bin'}:{env.get('PATH', '')}"
    return env


def which_ffmpeg(paths: RkvcPaths, env: dict[str, str]) -> tuple[str, str]:
    for ffmpeg, ffprobe in ((paths.ffmpeg, paths.ffprobe), ("ffmpeg", "ffprobe")):
        try:
            run([ffmpeg, "-version"], env=env)
            run([ffprobe, "-version"], env=env)
            return ffmpeg, ffprobe
        except RuntimeError:
            continue
    return "ffmpeg", "ffprobe"


def probe_video(ffprobe: str, path: str, env: dict[str, str]) -> VideoInfo:
    proc = run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,width,height,r_frame_rate,duration,nb_frames",
            "-show_entries",
            "format=duration,size,bit_rate",
            "-of",
            "json",
            path,
        ],
        env=env,
    )
    data = json.loads(proc.stdout or "{}")
    stream = (data.get("streams") or [{}])[0]
    fmt = data.get("format") or {}

    fps_s = stream.get("r_frame_rate") or "30/1"
    if "/" in fps_s:
        fps_num, fps_den = (int(x) for x in fps_s.split("/", 1))
    else:
        fps_num, fps_den = int(float(fps_s)), 1

    duration = float(stream.get("duration") or fmt.get("duration") or 0)
    frames = str(stream.get("nb_frames") or "0")
    kbps = float(
        actual_kbps(path, frames, duration, float(fps_num), float(fps_den))
    )

    return VideoInfo(
        path=path,
        width=int(stream.get("width") or 0),
        height=int(stream.get("height") or 0),
        fps_num=fps_num,
        fps_den=fps_den,
        duration=duration,
        codec=str(stream.get("codec_name") or "unknown"),
        bitrate_kbps=kbps,
    )


def clip_start_sec(info: VideoInfo, clip_sec: float, offset: str) -> float:
    if info.duration <= clip_sec:
        return 0.0
    if offset == "start":
        return 0.0
    if offset == "end":
        return max(0.0, info.duration - clip_sec)
    return max(0.0, (info.duration - clip_sec) / 2.0)


def bitrate_timeline(
    ffprobe: str, path: str, env: dict[str, str], bucket_sec: float = 1.0
) -> list[BitrateSegment]:
    proc = run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "packet=pts_time,size",
            "-of",
            "csv=p=0",
            path,
        ],
        env=env,
    )
    buckets: dict[int, int] = {}
    for line in (proc.stdout or "").splitlines():
        line = line.strip()
        if not line or "," not in line:
            continue
        pts_s, size_s = line.split(",", 1)
        try:
            pts = float(pts_s)
            size = int(size_s)
        except ValueError:
            continue
        if pts < 0:
            continue
        buckets[int(pts // bucket_sec)] = buckets.get(int(pts // bucket_sec), 0) + size

    if not buckets:
        info = probe_video(ffprobe, path, env)
        return [BitrateSegment(0.0, info.duration, info.bitrate_kbps)]

    return [
        BitrateSegment(
            idx * bucket_sec,
            (idx + 1) * bucket_sec,
            buckets[idx] * 8 / bucket_sec / 1000.0,
        )
        for idx in sorted(buckets)
    ]


def shift_timeline(
    tl: Sequence[BitrateSegment], offset: float, duration: float
) -> list[BitrateSegment]:
    out: list[BitrateSegment] = []
    for seg in tl:
        s, e = seg.start - offset, seg.end - offset
        if e <= 0 or s >= duration:
            continue
        out.append(BitrateSegment(max(0.0, s), min(duration, e), seg.kbps))
    return out


def _esc_drawtext(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace(":", "\\:")
        .replace("'", "\\'")
        .replace("%", "\\%")
    )


def _resolution_label(height: int) -> str:
    if height >= 2160:
        return "4K"
    if height >= 1080:
        return "1080p"
    if height >= 720:
        return "720p"
    return f"{height}p"


def sweep_split_x_expr(period: float) -> str:
    """与 blend 分界一致的横向位置（overlay/drawtext 用 main_w，blend 用 W）。"""
    p = max(period, 1.0)
    return f"(main_w*0.15)+(main_w*0.7)*(1+sin(2*PI*t/{p}))/2"


def sweep_blend_x_expr(period: float) -> str:
    p = max(period, 1.0)
    return f"(W*0.15)+(W*0.7)*(1+sin(2*PI*T/{p}))/2"


def build_overlay_filters(
    *,
    out_w: int,
    out_h: int,
    panel_h: int,
    src_timeline: Sequence[BitrateSegment],
    enc_timeline: Sequence[BitrateSegment],
    fontfile: str,
    fontcolor: str,
    duration: float,
    left_label: str,
    right_label: str,
    sweep_period: float,
) -> str:
    period = max(sweep_period, 1.0)
    filters: list[str] = []
    res = _resolution_label(panel_h)
    title_args = (
        f"fontfile={fontfile}:fontcolor={fontcolor}:fontsize=26:"
        "borderw=2:bordercolor=black@0.55:box=1:boxcolor=black@0.35:boxborderw=6"
    )
    meta_args = (
        f"fontfile={fontfile}:fontcolor={fontcolor}:fontsize=22:"
        "borderw=1:bordercolor=black@0.55:box=1:boxcolor=black@0.35:boxborderw=5"
    )
    stamp_args = (
        f"fontfile={fontfile}:fontcolor={fontcolor}:fontsize=20:"
        "borderw=1:bordercolor=black@0.55:box=0"
    )
    summary_args = (
        f"fontfile={fontfile}:fontcolor={fontcolor}:fontsize=22:"
        "borderw=1:bordercolor=black@0.55:box=1:boxcolor=black@0.35:boxborderw=6"
    )

    avg_src = sum(s.kbps for s in src_timeline) / max(len(src_timeline), 1)
    avg_enc = sum(s.kbps for s in enc_timeline) / max(len(enc_timeline), 1)
    bw_ratio = avg_src / avg_enc if avg_enc > 0 else 0.0
    summary = f"传输码率 {avg_src:.0f}→{avg_enc:.0f} kbps  带宽比 {bw_ratio:.1f}x"
    filters.append(
        f"drawtext={summary_args}:x=(w-text_w)/2:y=10:text='"
        + _esc_drawtext(summary)
        + "'"
    )

    for i in range(max(1, int(math.ceil(duration)))):
        t0, t1 = float(i), min(float(i + 1), duration)
        if t1 <= t0:
            break
        stamp = f"{i // 3600:02d}:{(i % 3600) // 60:02d}:{i % 60:02d}"
        filters.append(
            f"drawtext={stamp_args}:x=(w-text_w)/2:y=38:text='"
            + _esc_drawtext(stamp)
            + f"':enable='between(t,{t0:.3f},{t1:.3f})'"
        )

    filters += [
        f"drawtext={title_args}:x=20:y=66:text='" + _esc_drawtext(left_label) + "'",
        f"drawtext={meta_args}:x=20:y=94:text='" + _esc_drawtext(res) + "'",
        f"drawtext={title_args}:x=w-text_w-20:y=66:text='"
        + _esc_drawtext(right_label)
        + "'",
    ]

    def bitrates(tl: Sequence[BitrateSegment], x_expr: str) -> None:
        for seg in tl:
            if seg.start >= duration:
                break
            t0, t1 = seg.start, min(seg.end, duration)
            if t1 <= t0:
                continue
            filters.append(
                f"drawtext={meta_args}:x={x_expr}:y=122:text='"
                + _esc_drawtext(f"码率: {seg.kbps:.0f} kbps")
                + f"':enable='between(t,{t0:.3f},{t1:.3f})'"
            )

    bitrates(src_timeline, "20")
    bitrates(enc_timeline, "w-text_w-20")

    return ",".join(filters)


def render_comparison(
    ffmpeg: str,
    ffprobe: str,
    env: dict[str, str],
    *,
    left: str,
    right: str,
    output: str,
    left_start: float,
    right_start: float,
    duration: float,
    panel_height: int,
    display_height: int,
    fontfile: str,
    fontcolor: str,
    left_label: str,
    right_label: str,
    left_bitrate_path: str | None = None,
    right_bitrate_path: str | None = None,
    left_bitrate_start: float | None = None,
    enc_scale_denom: int = 3,
    sweep_period: float = DEFAULT_SWEEP_PERIOD,
) -> None:
    left_info = probe_video(ffprobe, left, env)
    right_info = probe_video(ffprobe, right, env)

    out_h = panel_height if panel_height > 0 else display_height
    out_w = max(2, int(out_h * 16 / 9))
    if out_w % 2:
        out_w += 1

    br_offset = left_bitrate_start if left_bitrate_start is not None else left_start
    left_tl = shift_timeline(
        bitrate_timeline(ffprobe, left_bitrate_path or left, env), br_offset, duration
    )
    right_tl = shift_timeline(
        bitrate_timeline(ffprobe, right_bitrate_path or right, env), right_start, duration
    )
    if not left_tl:
        left_tl = [BitrateSegment(0.0, duration, left_info.bitrate_kbps)]
    if not right_tl:
        right_tl = [BitrateSegment(0.0, duration, right_info.bitrate_kbps)]

    overlay = build_overlay_filters(
        out_w=out_w,
        out_h=out_h,
        panel_h=display_height,
        src_timeline=left_tl,
        enc_timeline=right_tl,
        fontfile=fontfile,
        fontcolor=fontcolor,
        duration=duration,
        left_label=left_label,
        right_label=right_label,
        sweep_period=sweep_period,
    )
    split_x = sweep_blend_x_expr(sweep_period)
    line_x = sweep_split_x_expr(sweep_period)
    scale_pad = (
        f"scale={out_w}:{out_h}:force_original_aspect_ratio=decrease,"
        f"pad={out_w}:{out_h}:(ow-iw)/2:(oh-ih)/2,setsar=1"
    )
    # drawbox 在 ffmpeg 4.x 默认不逐帧求值；用 overlay+eval=frame 绘制移动分割线
    filter_complex = (
        f"color=c=white:s=3x{out_h}:d={duration:.3f},format=rgba[sweepline];"
        f"[0:v]trim=start={left_start:.3f}:duration={duration:.3f},setpts=PTS-STARTPTS,"
        f"{scale_pad}[left];"
        f"[1:v]trim=start={right_start:.3f}:duration={duration:.3f},setpts=PTS-STARTPTS,"
        f"{scale_pad}[right];"
        f"[left][right]blend=all_expr='if(lt(X,{split_x}),A,B)'[blended];"
        f"[blended][sweepline]overlay=x='{line_x}-1':y=0:eval=frame[stacked];"
        f"[stacked]{overlay}[vout]"
    )
    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)
    run(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            left,
            "-i",
            right,
            "-filter_complex",
            filter_complex,
            "-map",
            "[vout]",
            "-an",
            "-c:v",
            "libx264",
            "-preset",
            "medium",
            "-crf",
            "16",
            "-movflags",
            "+faststart",
            output,
        ],
        env=env,
    )


def align_even(v: int) -> int:
    return v & ~1 if v > 0 else 0


def enc_dims(width: int, height: int, denom: int) -> tuple[int, int]:
    if denom <= 1:
        return align_even(width), align_even(height)
    return align_even(width // denom), align_even(height // denom)


def extract_clip_mp4(
    ffmpeg: str,
    src: str,
    dst: str,
    *,
    start: float,
    duration: float,
    env: dict[str, str],
) -> None:
    """截取源片片段，流拷贝不重编码。"""
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    run(
        [
            ffmpeg,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-ss",
            f"{start:.3f}",
            "-t",
            f"{duration:.3f}",
            "-i",
            src,
            "-an",
            "-c",
            "copy",
            "-movflags",
            "+faststart",
            dst,
        ],
        env=env,
    )


def extract_clip_nv12(
    ffmpeg: str,
    ffprobe: str,
    src: str,
    dst_nv12: str,
    *,
    start: float,
    duration: float,
    display_height: int,
    env: dict[str, str],
) -> tuple[int, int, int, int, int]:
    os.makedirs(os.path.dirname(dst_nv12) or ".", exist_ok=True)
    meta = json.loads(
        run(
            [
                ffprobe,
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height,r_frame_rate",
                "-of",
                "json",
                src,
            ],
            env=env,
        ).stdout
        or "{}"
    )
    st = (meta.get("streams") or [{}])[0]
    sw, sh = int(st.get("width") or 1920), int(st.get("height") or 1080)
    fps_s = st.get("r_frame_rate") or "30/1"
    if "/" in fps_s:
        fps_num, fps_den = (int(x) for x in fps_s.split("/", 1))
    else:
        fps_num, fps_den = int(float(fps_s)), 1

    dh = display_height
    dw = align_even(int(sw * dh / sh))
    if dw % 2:
        dw += 1

    run(
        [
            ffmpeg,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-ss",
            f"{start:.3f}",
            "-t",
            f"{duration:.3f}",
            "-i",
            src,
            "-an",
            "-vf",
            f"scale={dw}:{dh}:flags=lanczos,format=nv12",
            "-f",
            "rawvideo",
            dst_nv12,
        ],
        env=env,
    )
    frame_bytes = dw * dh * 3 // 2
    frames = os.path.getsize(dst_nv12) // frame_bytes if frame_bytes else 0
    return dw, dh, fps_num, fps_den, frames


def nv12_to_y4m(
    ffmpeg: str,
    nv12: str,
    y4m: str,
    width: int,
    height: int,
    fps_num: int,
    fps_den: int,
    env: dict[str, str],
) -> None:
    run(
        [
            ffmpeg,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "nv12",
            "-video_size",
            f"{width}x{height}",
            "-framerate",
            f"{fps_num}/{fps_den}",
            "-i",
            nv12,
            "-pix_fmt",
            "yuv420p",
            "-f",
            "yuv4mpegpipe",
            y4m,
        ],
        env=env,
    )


def downscale_nv12(
    paths: RkvcPaths,
    in_nv12: str,
    out_nv12: str,
    sw: int,
    sh: int,
    dw: int,
    dh: int,
    frames: int,
    env: dict[str, str],
) -> None:
    run(
        [
            paths.rkvc_yuv_upscale,
            "--in",
            in_nv12,
            "--out",
            out_nv12,
            "--sw",
            str(sw),
            "--sh",
            str(sh),
            "--dw",
            str(dw),
            "--dh",
            str(dh),
            "--algo",
            "bilinear",
            "--pix-fmt",
            "nv12",
            "--frames",
            str(frames),
        ],
        env=env,
    )


def svt_encode_args(
    paths: RkvcPaths,
    mode: str,
    target_kbps: int,
    *,
    rate_mode: str,
) -> list[str]:
    # 演示视频默认 VBR，确保实测码率贴近目标，压缩比可读
    if rate_mode == "vbr":
        return ["--rc", "1", "--tbr", str(target_kbps)]
    cfg = load_config(paths.bench_config, paths.project_root)
    if mode == "low":
        if cfg.get("svt", {}).get("rd_mode") == "vbr":
            return ["--rc", "1", "--tbr", str(target_kbps)]
        qp = lookup_calibration(cfg, "svt_av1.low_qp", target_kbps)
        return ["--rc", "0", "--aq-mode", "0", "--qp", str(qp)]
    if cfg.get("svt", {}).get("rd_mode") == "vbr":
        return ["--rc", "1", "--tbr", str(target_kbps)]
    crf = lookup_calibration(cfg, "svt_av1.full_crf", target_kbps)
    return ["--rc", "0", "--crf", str(crf)]


def svt_encode(
    paths: RkvcPaths,
    y4m: str,
    ivf: str,
    *,
    target_kbps: int,
    mode: str,
    frames: int,
    gop: int,
    env: dict[str, str],
    rate_mode: str = "vbr",
) -> None:
    cfg = load_config(paths.bench_config, paths.project_root)
    svt = cfg.get("svt", {})
    run(
        [
            paths.svt_enc,
            "--input",
            y4m,
            "-b",
            ivf,
            "--preset",
            str(svt.get("preset", 11)),
            *svt_encode_args(paths, mode, target_kbps, rate_mode=rate_mode),
            "--keyint",
            str(gop),
            "--lp",
            str(svt.get("lp", 4)),
            "-n",
            str(frames),
        ],
        env=env,
    )


def session_decode_upscale(
    paths: RkvcPaths,
    bitstream: str,
    out_nv12: str,
    width: int,
    height: int,
    enc_scale_denom: int,
    env: dict[str, str],
) -> None:
    run(
        [
            paths.rkvc_session_upscale,
            "-i",
            bitstream,
            "-o",
            out_nv12,
            "--width",
            str(width),
            "--height",
            str(height),
            "--enc-scale-denom",
            str(enc_scale_denom),
            "--post-upscale",
            "rkvc_sr",
            "--rkvc-sr-model",
            paths.rkvc_sr_model,
        ],
        env=env,
    )


def nv12_to_mp4(
    ffmpeg: str,
    nv12: str,
    mp4: str,
    width: int,
    height: int,
    fps_num: int,
    fps_den: int,
    env: dict[str, str],
) -> None:
    # 优先系统 ffmpeg 的 libx264 做无损性较低的中间封装；项目 ffmpeg 可能无 libx264。
    enc_ffmpeg = "ffmpeg"
    try:
        run(["ffmpeg", "-hide_banner", "-encoders"], env=env, check=False)
    except RuntimeError:
        enc_ffmpeg = ffmpeg
    run(
        [
            enc_ffmpeg,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "nv12",
            "-video_size",
            f"{width}x{height}",
            "-framerate",
            f"{fps_num}/{fps_den}",
            "-i",
            nv12,
            "-an",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "12",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            mp4,
        ],
        env=env,
    )


def require_tools(paths: RkvcPaths) -> None:
    missing = [
        f"{label}: {p}"
        for label, p in (
            ("ffmpeg", paths.ffmpeg),
            ("ffprobe", paths.ffprobe),
            ("SvtAv1EncApp", paths.svt_enc),
            ("rkvc_yuv_upscale", paths.rkvc_yuv_upscale),
            ("rkvc_session_upscale", paths.rkvc_session_upscale),
            ("rkvc_sr_model", paths.rkvc_sr_model),
        )
        if not p or not Path(p).exists()
    ]
    if missing:
        raise RuntimeError("演示管道缺少依赖:\n" + "\n".join(missing))


def make_demo(
    *,
    input_path: str,
    output_path: str,
    target_kbps: int,
    reference_kbps: int,
    reference_mode: str,
    clip_sec: float,
    clip_offset: str,
    display_height: int,
    enc_scale_denom: int,
    panel_height: int,
    gop: int,
    fontfile: str,
    fontcolor: str,
    work_dir: str | None,
    keep_intermediate: bool,
    rate_mode: str = "vbr",
    sweep_period: float = DEFAULT_SWEEP_PERIOD,
    right_label: str = DEFAULT_RIGHT_LABEL,
) -> dict:
    paths = load_rkvc_paths()
    require_tools(paths)
    env = ld_env(paths)
    ffmpeg, ffprobe = which_ffmpeg(paths, env)

    info = probe_video(ffprobe, input_path, env)
    use_sec = min(clip_sec, info.duration) if clip_sec > 0 else info.duration
    start = clip_start_sec(info, use_sec, clip_offset)

    tmp_ctx = (
        tempfile.TemporaryDirectory(prefix="rkvc-demo-") if work_dir is None else None
    )
    base = Path(work_dir or tmp_ctx.name)  # type: ignore[union-attr]
    base.mkdir(parents=True, exist_ok=True)

    ref_nv12 = str(base / "ref.nv12")
    width, height, fps_num, fps_den, frames = extract_clip_nv12(
        ffmpeg,
        ffprobe,
        input_path,
        ref_nv12,
        start=start,
        duration=use_sec,
        display_height=display_height,
        env=env,
    )
    if frames <= 0:
        frames = max(1, int(use_sec * fps_num / fps_den))

    enc_w, enc_h = enc_dims(width, height, enc_scale_denom)
    ref_ivf = str(base / "reference.ivf")
    ref_mp4 = str(base / "reference.mp4")
    clip_mp4 = str(base / "clip.mp4")
    low_ivf = str(base / "compressed.ivf")
    up_nv12 = str(base / "upscaled.nv12")
    right_mp4 = str(base / "compressed.mp4")
    enc_nv12 = str(base / "enc_lo.nv12")
    enc_y4m = str(base / "enc_lo.y4m")
    ref_y4m = str(base / "ref_full.y4m")

    if reference_mode == "fair":
        nv12_to_y4m(ffmpeg, ref_nv12, ref_y4m, width, height, fps_num, fps_den, env)
        svt_encode(
            paths,
            ref_y4m,
            ref_ivf,
            target_kbps=reference_kbps,
            mode="full",
            frames=frames,
            gop=gop,
            env=env,
            rate_mode=rate_mode,
        )
        nv12_to_mp4(ffmpeg, ref_nv12, ref_mp4, width, height, fps_num, fps_den, env)
        left_path, left_br = ref_mp4, ref_ivf
        left_br_start = 0.0
        ref_kbps = probe_video(ffprobe, ref_ivf, env).bitrate_kbps
        left_label = "AV1 参考"
    else:
        # 原片：仅截取，不重编码
        extract_clip_mp4(
            ffmpeg, input_path, clip_mp4, start=start, duration=use_sec, env=env
        )
        left_path, left_br = clip_mp4, input_path
        left_br_start = start
        ref_kbps = probe_video(ffprobe, input_path, env).bitrate_kbps
        left_label = "原片"

    downscale_nv12(
        paths, ref_nv12, enc_nv12, width, height, enc_w, enc_h, frames, env
    )
    nv12_to_y4m(ffmpeg, enc_nv12, enc_y4m, enc_w, enc_h, fps_num, fps_den, env)
    svt_encode(
        paths,
        enc_y4m,
        low_ivf,
        target_kbps=target_kbps,
        mode="low",
        frames=frames,
        gop=gop,
        env=env,
        rate_mode=rate_mode,
    )
    session_decode_upscale(
        paths, low_ivf, up_nv12, width, height, enc_scale_denom, env
    )
    nv12_to_mp4(ffmpeg, up_nv12, right_mp4, width, height, fps_num, fps_den, env)

    render_comparison(
        ffmpeg,
        ffprobe,
        env,
        left=left_path,
        right=right_mp4,
        output=output_path,
        left_start=0.0,
        right_start=0.0,
        duration=use_sec,
        panel_height=panel_height or display_height,
        display_height=display_height,
        fontfile=fontfile,
        fontcolor=fontcolor,
        left_label=left_label,
        right_label=right_label,
        left_bitrate_path=left_br,
        left_bitrate_start=left_br_start,
        right_bitrate_path=low_ivf,
        enc_scale_denom=enc_scale_denom,
        sweep_period=sweep_period,
    )

    enc_kbps = probe_video(ffprobe, low_ivf, env).bitrate_kbps
    bw_ratio = ref_kbps / enc_kbps if enc_kbps > 0 else 0.0
    result = {
        "scheme": "av1-up3x-rkvc_sr",
        "input": input_path,
        "output": output_path,
        "clip_start": start,
        "clip_sec": use_sec,
        "display_resolution": f"{width}x{height}",
        "encode_resolution": f"{enc_w}x{enc_h}",
        "enc_scale_denom": enc_scale_denom,
        "reference_kbps": reference_kbps,
        "target_kbps": target_kbps,
        "reference_mode": reference_mode,
        "rate_mode": rate_mode,
        "source_bitrate_kbps": round(ref_kbps, 1),
        "encoded_bitrate_kbps": round(enc_kbps, 1),
        "bandwidth_ratio": round(bw_ratio, 2),
    }

    if not keep_intermediate:
        for p in base.iterdir():
            try:
                p.unlink()
            except OSError:
                pass
    if tmp_ctx is not None:
        tmp_ctx.cleanup()

    return result


def load_batch_config(path: str) -> list[dict]:
    data = json.loads(Path(path).read_text())
    defaults = data.get("defaults") or {}
    return [{**defaults, **item} for item in (data.get("videos") or data.get("items") or [])]


def demo_kwargs(item: dict, args: argparse.Namespace) -> dict:
    return {
        "target_kbps": int(item.get("target_kbps", args.target_kbps)),
        "reference_kbps": int(item.get("reference_kbps", args.reference_kbps)),
        "reference_mode": str(item.get("reference_mode", args.reference_mode)),
        "clip_sec": float(item.get("clip_sec", args.clip_sec)),
        "clip_offset": str(item.get("clip_offset", args.clip_offset)),
        "display_height": int(item.get("display_height", args.display_height)),
        "enc_scale_denom": int(item.get("enc_scale_denom", args.enc_scale_denom)),
        "panel_height": int(item.get("panel_height", args.panel_height)),
        "gop": int(item.get("gop", args.gop)),
        "fontfile": str(item.get("font", args.font)),
        "fontcolor": str(item.get("font_color", args.font_color)),
        "rate_mode": str(item.get("rate_mode", args.rate_mode)),
        "sweep_period": float(item.get("sweep_period", args.sweep_period)),
        "right_label": str(item.get("right_label", args.right_label)),
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="生成 AV1 + 3×AI 上采样左右对比演示视频（1080p）"
    )
    p.add_argument("-i", "--input", help="源视频")
    p.add_argument("-o", "--output", help="输出 MP4")
    p.add_argument("--config", help="批量配置 JSON（bench/demo_videos.json）")
    p.add_argument("--target-kbps", type=int, default=DEFAULT_TARGET_KBPS)
    p.add_argument("--reference-kbps", type=int, default=DEFAULT_REFERENCE_KBPS)
    p.add_argument(
        "--reference-mode",
        choices=("fair", "source"),
        default="source",
        help="source=原片截取不重编码（默认）；fair=1080p AV1 高码率参考",
    )
    p.add_argument("--clip-sec", type=float, default=10.0)
    p.add_argument(
        "--clip-offset",
        choices=("middle", "start", "end"),
        default="middle",
    )
    p.add_argument("--display-height", type=int, default=DEFAULT_DISPLAY_HEIGHT)
    p.add_argument("--panel-height", type=int, default=DEFAULT_DISPLAY_HEIGHT)
    p.add_argument("--enc-scale-denom", type=int, default=DEFAULT_ENC_SCALE_DENOM)
    p.add_argument("--gop", type=int, default=30)
    p.add_argument(
        "--rate-mode",
        choices=("vbr", "calibrated"),
        default="vbr",
        help="SVT 码控：vbr 命中目标码率（演示默认）；calibrated 用 RD 校准表",
    )
    p.add_argument(
        "--sweep-period",
        type=float,
        default=DEFAULT_SWEEP_PERIOD,
        help="对比分界线左右扫动周期（秒，默认 8）",
    )
    p.add_argument(
        "--right-label",
        default=DEFAULT_RIGHT_LABEL,
        help="右侧叠加标题",
    )
    p.add_argument("--font", default=DEFAULT_FONT)
    p.add_argument("--font-color", default=DEFAULT_FONT_COLOR)
    p.add_argument("--work-dir", help="保留中间文件")
    p.add_argument("--keep-intermediate", action="store_true")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not Path(args.font).is_file():
        print(f"warning: font not found: {args.font}", file=sys.stderr)

    results: list[dict] = []
    common = {
        "work_dir": args.work_dir,
        "keep_intermediate": args.keep_intermediate,
    }

    if args.config:
        for item in load_batch_config(args.config):
            inp = item.get("input") or item.get("src")
            if not inp:
                continue
            out = item.get("output") or str(
                _BENCH_DIR / "results" / "demos" / f"{Path(inp).stem}_comparison.mp4"
            )
            print(f"[demo] {inp} -> {out}")
            results.append(
                make_demo(
                    input_path=inp,
                    output_path=out,
                    **demo_kwargs(item, args),
                    **common,
                )
            )
            print(json.dumps(results[-1], ensure_ascii=False, indent=2))
    else:
        if not args.input or not args.output:
            print("error: -i and -o required without --config", file=sys.stderr)
            return 2
        res = make_demo(
            input_path=args.input,
            output_path=args.output,
            **demo_kwargs({}, args),
            **common,
        )
        results.append(res)
        print(json.dumps(res, ensure_ascii=False, indent=2))

    return 0 if results else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
