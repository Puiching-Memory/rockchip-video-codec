#!/usr/bin/env bash
# tools/bench/run_rd_benchmark.sh — RK3588 端到端 RD 基准（集成于 rkvc 项目）
#
# 对比路线（默认）: h264 / h265 / svt-av1 / svt-av1-hq / rkvc / post-upscale
# 实验路线（搁置）: svt-av1+superres — AV1 内建 superres，见 tools/bench/README.md
#
# 用法:
#   ./tools/bench/run_rd_benchmark.sh [源视频.mp4]
#   RUN_CODECS=h264,rkvc ./tools/bench/run_rd_benchmark.sh clip.mp4
#   PLOT_ONLY=1 ./tools/bench/run_rd_benchmark.sh

set -euo pipefail

BENCH_ROOT="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$BENCH_ROOT/../.." && pwd)"
TOOLS_PY="$PROJECT_ROOT/.venv/bin/python"
if [[ ! -x "$TOOLS_PY" ]]; then
    echo "[error] 共享 Python 环境不存在: $TOOLS_PY" >&2
    echo "请先在仓库根目录运行: uv sync" >&2
    exit 1
fi
BENCH_TOOLS="$BENCH_ROOT/tools"
BENCH_CONFIG="${BENCH_CONFIG:-$BENCH_ROOT/config.json}"
RESULTS="$BENCH_ROOT/results"
WORKDIR="$BENCH_ROOT/work"

# 从 tools/bench/config.json 加载默认值；环境变量可覆盖（在调用前 export）。
load_bench_config() {
    if [[ ! -f "$BENCH_CONFIG" ]]; then
        echo "[error] 配置文件不存在: $BENCH_CONFIG" >&2
        exit 1
    fi
    "$TOOLS_PY" "$BENCH_TOOLS/config.py" validate "$BENCH_CONFIG" "$PROJECT_ROOT" >/dev/null
    local line key val
    while IFS= read -r line; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        key="${line%%=*}"
        val="${line#*=}"
        if [[ -z "${!key+x}" ]]; then
            export "$key=$val"
        fi
    done < <("$TOOLS_PY" "$BENCH_TOOLS/config.py" defaults "$BENCH_CONFIG" "$PROJECT_ROOT")
}

SRC_VIDEO="${1:-${SRC_VIDEO:-}}"
CLIP_START_SEC="${CLIP_START_SEC:-}"

# shellcheck source=../scripts/build-common.sh
source "$PROJECT_ROOT/scripts/build-common.sh" 2>/dev/null || true

# SoC 名（供 config.py {soc} 模板与下方 MLVC 模型默认值共用）
RKVC_SOC="${RKVC_SOC:-$(rkvc_soc_name 2>/dev/null || true)}"
export RKVC_SOC

load_bench_config

case "$RKVC_BUILD" in /*) ;; *) RKVC_BUILD="$PROJECT_ROOT/$RKVC_BUILD" ;; esac
RAM_WORK_DIR="$RAMDISK_DIR/work"

CLIP_SEC="${CLIP_SEC:-4}"
CLIP_OFFSET="${CLIP_OFFSET:-middle}"
TARGET_KBPS="${TARGET_KBPS:-1000}"
IFS=',' read -ra BITRATES <<< "$TARGET_KBPS"

require_project_ffmpeg() {
    if [[ ! -x "$FFMPEG" || ! -x "$FFPROBE" ]]; then
        echo "[error] 项目 ffmpeg 未构建或不可执行:" >&2
        echo "  FFMPEG=$FFMPEG" >&2
        echo "  FFPROBE=$FFPROBE" >&2
        echo "请运行: ./scripts/rebuild-ffmpeg-rkmpp.sh" >&2
        exit 1
    fi
}

require_project_ffmpeg
PREP_FFMPEG="${PREP_FFMPEG:-$FFMPEG}"

SVT_ENC="${SVT_ENC:-$SVT_PREFIX/bin/SvtAv1EncApp}"
RKVC_TRANS="$RKVC_BUILD/rkvc_transcode"
RKVC_YUV_UPSCALE="$RKVC_BUILD/rkvc_yuv_upscale"
RKVC_SESSION_UPSCALE="$RKVC_BUILD/rkvc_session_upscale"
RKVC_SR_MODEL="${RKVC_SR_MODEL:-$PROJECT_ROOT/models/rkvc_sr_x3.crypt.rknn}"
export RKVC_SR_MODEL
RKVC_ENC="$RKVC_BUILD/rkvc_encode"

# MLVC 神经编解码（-p neural）：模型/PMF 路径与分辨率默认由 config.json
# （mlvc.*，{soc} 占位符按探测 SoC 展开）经 config.py 导出；env 可覆盖。
MLVC_ENC_MODEL="${MLVC_ENC_MODEL:-$PROJECT_ROOT/models/mlvc/MLVCEncoder_${RKVC_SOC}.rknn}"
MLVC_DEC_MODEL="${MLVC_DEC_MODEL:-$PROJECT_ROOT/models/mlvc/MLVCDecoder_${RKVC_SOC}.rknn}"

# codec/policy 清单单一来源：新增时改这里 + config.json + plot_rd_curve.py
POST_UPSCALE_BASE_LIST=(h264 h265 svt-av1 svt-av1-hq)
POST_UPSCALE_BASE_RE="($(IFS='|'; echo "${POST_UPSCALE_BASE_LIST[*]}"))"
RKVC_ALL_POLICIES=(realtime balanced quality offline neural)
ALL_KNOWN_CODECS=(h264 h265 svt-av1 svt-av1-hq svt-av1+superres rkvc rkvc-realtime rkvc-balanced rkvc-quality rkvc-offline rkvc-neural)

FFMPEG_LIB_DIRS=""
for _d in "$FFMPEG_SRC"/libav* "$FFMPEG_SRC"/libsw* "$FFMPEG_SRC"/libpostproc; do
    [[ -d "$_d" ]] && FFMPEG_LIB_DIRS="${_d}:${FFMPEG_LIB_DIRS}"
done
export LD_LIBRARY_PATH="$MPP_LIB:${FFMPEG_LIB_DIRS}$SVT_PREFIX/lib:${RKVC_BUILD}:${LD_LIBRARY_PATH:-}"
export PATH="$SVT_PREFIX/bin:${FFMPEG%/*}:$PATH"

mkdir -p "$RESULTS" "$WORKDIR" "$RAMDISK_DIR" "$RAM_WORK_DIR"

CLIP_MP4="$RAMDISK_DIR/clip.mp4"
REF_Y4M_RAM="$RAMDISK_DIR/clip.y4m"
REF_YUV_RAM="$RAMDISK_DIR/clip.yuv"
REF_NV12_RAM="$RAMDISK_DIR/clip.nv12"
CLIP_META="$WORKDIR/clip.meta"
CSV="$RESULTS/rd_data.csv"
SESSION_META="$RESULTS/session.meta"
SESSION_CODECS="$RESULTS/session.codecs"
CSV_HEADER="codec,target_kbps,actual_kbps,psnr_y,psnr_u,psnr_v,psnr_avg,ssim,encode_sec,decode_sec,rga_sec,write_sec,postproc_sec"
CSV_FIELDS="$CSV_HEADER"

# 码流/YUV 走 tmpfs；日志与 stats 留在 work/ 便于排查。
bench_ramdir() {
    echo "$RAM_WORK_DIR/$1/$2"
}

bench_logdir() {
    echo "$WORKDIR/logs/$1/$2"
}

# 测完即删大体积 YUV，避免 tmpfs 被撑满（单帧 1080p YUV ~3MB × 62 帧 × 多路线）。
bench_cleanup_ramdir() {
    local ramdir="$1"
    rm -f "$ramdir"/decoded.yuv "$ramdir"/decoded.nv12 \
        "$ramdir"/decoded_lo.yuv "$ramdir"/decoded_lo.nv12 \
        "$ramdir"/upscaled.yuv "$ramdir"/upscaled.nv12 \
        "$ramdir"/stream.mp4 "$ramdir"/stream.ivf
}

usage() {
    cat <<EOF
用法: $(basename "$0") [源视频.mp4]

默认从 tools/bench/config.json 读取路径、码率点、RD 校准表等；环境变量可覆盖单项。

常用覆盖:
  BENCH_CONFIG   配置文件路径（默认 $BENCH_ROOT/config.json）
  SRC_VIDEO      源视频路径（也可作为第一个参数）
  RUN_CODECS     见 config.json run.codecs
  TARGET_KBPS    逗号分隔 kbps 列表
  ENC_SCALE_DENOM / UPSCALE_ALGOS / CLIP_SEC / CLIP_OFFSET
  RKVC_BUILD     rkvc 构建目录
  PLOT_ONLY=1    仅根据已有 CSV 绘图
  BENCH_CSV_MODE session（默认）| accumulate

前置条件:
  ./scripts/build-svt.sh
  ./scripts/rebuild-ffmpeg-rkmpp.sh
  cmake -B .build/release && cmake --build .build/release
EOF
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && { usage; exit 0; }

align_even() {
    local v="$1"
    echo $(( v & ~1 ))
}

enc_dims() {
    ENC_W=$(align_even $(( WIDTH / ENC_SCALE_DENOM )))
    ENC_H=$(align_even $(( HEIGHT / ENC_SCALE_DENOM )))
}

load_session_meta_for_plot() {
  [[ -f "$SESSION_META" ]] || return 0
  local line key val
  while IFS= read -r line; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    key="${line%%=*}"
    val="${line#*=}"
    case "$key" in
      clip_sec) CLIP_SEC="$val" ;;
      clip_start) CLIP_START="$val" ;;
      frames) FRAMES="$val" ;;
      width) WIDTH="$val" ;;
      height) HEIGHT="$val" ;;
      enc_scale_denom) ENC_SCALE_DENOM="$val" ;;
    esac
  done <"$SESSION_META"
}

plot_results() {
    local frames="${1:-62}"
    local title_rd title_perf
    local plot_extra=()
    local has_upscale=0
    load_session_meta_for_plot
    if [[ -n "${FRAMES:-}" ]]; then
        frames="$FRAMES"
    fi
    if [[ -f "$SESSION_CODECS" ]]; then
        plot_extra+=(--session-codecs "$SESSION_CODECS")
        if grep -E '\+up[0-9]+x-' "$SESSION_CODECS" >/dev/null 2>&1; then
            has_upscale=1
        fi
    fi
    # CSV 里也可能有 upscale 路线（accumulate 模式）
    if [[ "$has_upscale" -eq 0 && -f "$CSV" ]] && \
            awk -F, 'NR>1 && $1 ~ /\+up[0-9]+x-/ {found=1; exit} END{exit !found}' "$CSV"; then
        has_upscale=1
    fi

    if [[ "$has_upscale" -eq 1 && -n "${WIDTH:-}" && "${WIDTH}" -gt 0 ]]; then
        enc_dims
        title_rd="E2E RD Curve (${CLIP_SEC}s@${CLIP_START}s, ${WIDTH}x${HEIGHT} vs ${ENC_W}x${ENC_H} + ${ENC_SCALE_DENOM}x upscale)"
        title_perf="E2E Performance (${CLIP_SEC}s@${CLIP_START}s, 1080p vs ${ENC_H}p+${ENC_SCALE_DENOM}x up)"
    elif [[ -n "${WIDTH:-}" && "${WIDTH}" -gt 0 ]]; then
        title_rd="E2E RD Curve (${CLIP_SEC}s@${CLIP_START}s, ${WIDTH}x${HEIGHT})"
        title_perf="E2E Performance (${CLIP_SEC}s@${CLIP_START}s, ${WIDTH}x${HEIGHT})"
    else
        title_rd="E2E RD Curve (${CLIP_SEC}s@${CLIP_START:-0}s)"
        title_perf="E2E Performance (${CLIP_SEC}s@${CLIP_START:-0}s)"
    fi

    (cd "$BENCH_ROOT" && "$TOOLS_PY" plot_rd_curve.py --csv "$CSV" \
        --out "$RESULTS/rd_curve_e2e" --title "$title_rd" "${plot_extra[@]}")
    (cd "$BENCH_ROOT" && "$TOOLS_PY" plot_perf.py --csv "$CSV" \
        --out "$RESULTS/perf_e2e" --frames "$frames" --title "$title_perf" \
        "${plot_extra[@]}")
}

if [[ "${PLOT_ONLY:-0}" == "1" ]]; then
    plot_results "${FRAMES:-62}"
    exit 0
fi

if [[ -z "$SRC_VIDEO" ]]; then
    echo "[error] 请指定源视频: $0 /path/to/video.mp4 或设置 SRC_VIDEO" >&2
    usage
    exit 1
fi
if [[ ! -f "$SRC_VIDEO" ]]; then
    echo "[error] 源视频不存在: $SRC_VIDEO" >&2
    exit 1
fi

sync_ref_to_ram() {
    : # 参考帧已在 RAMDISK_DIR 生成，无需再同步
}

clip_meta_key() {
    local mtime
    mtime=$(stat -c %Y "$SRC_VIDEO" 2>/dev/null || echo 0)
    # 容器输入改为 YUV 重封装（禁止 -c copy 负 PTS）
    echo "${CLIP_SEC}|${CLIP_OFFSET}|${CLIP_START}|${SRC_VIDEO}|${mtime}"
}

compute_clip_start() {
    if [[ -n "$CLIP_START_SEC" ]]; then
        CLIP_START="$CLIP_START_SEC"
        return
    fi
    if [[ "$CLIP_OFFSET" == "start" ]]; then
        CLIP_START=0
        return
    fi
    local dur
    dur=$(probe_src_duration_sec)
    CLIP_START=$("$TOOLS_PY" "$BENCH_TOOLS/clip.py" start "$dur" "$CLIP_SEC")
}

src_is_raw_elementary() {
    case "${SRC_VIDEO##*.}" in
        h265|hevc|265|h264|264|avc) return 0 ;;
        *) return 1 ;;
    esac
}

prep_input_format_args() {
    case "${SRC_VIDEO##*.}" in
        h265|hevc|265) echo "-f hevc" ;;
        h264|264|avc)  echo "-f h264" ;;
        *) echo "" ;;
    esac
}

# 裸码流与容器均使用项目 ffmpeg-rockchip（须含 h264/hevc decoder + parser）。
resolve_demux_ffmpeg() {
    echo "$FFMPEG"
}

resolve_demux_ffprobe() {
    echo "$FFPROBE"
}

# 源视频时长（秒）；裸码流首次 count_frames 较慢，结果缓存在 work/。
probe_src_duration_sec() {
    local cache_key
    cache_key=$("$TOOLS_PY" "$BENCH_TOOLS/clip.py" cache-key "$SRC_VIDEO")
    local cache="$WORKDIR/src_duration_${cache_key}.cache"
    local mtime cached_mtime cached_dur
    mtime=$(stat -c %Y "$SRC_VIDEO" 2>/dev/null || echo 0)
    if [[ -f "$cache" ]]; then
        read -r cached_mtime cached_dur < "$cache" || true
        if [[ "$cached_mtime" == "$mtime" && -n "$cached_dur" ]]; then
            echo "$cached_dur"
            return
        fi
    fi

    local demux_probe dur
    local -a prep_in
    demux_probe=$(resolve_demux_ffprobe)
    prep_in=()
    read -ra prep_in <<< "$(prep_input_format_args)"
    dur=$("$demux_probe" -v error "${prep_in[@]}" -show_entries format=duration -of csv=p=0 \
        "$SRC_VIDEO" 2>/dev/null || true)

    if [[ -z "$dur" || "$dur" == "N/A" ]]; then
        if src_is_raw_elementary; then
            echo "[prep] 裸码流无 duration，count_frames 探测时长（首次较慢，已缓存）..." >&2
            local probe_out fps_s frames
            probe_out=$("$demux_probe" -v error "${prep_in[@]}" -select_streams v:0 -count_frames \
                -show_entries stream=nb_read_frames,r_frame_rate -of csv=p=0 "$SRC_VIDEO" 2>/dev/null || true)
            fps_s=$(echo "$probe_out" | cut -d, -f1)
            frames=$(echo "$probe_out" | cut -d, -f2)
            dur=$(FPS_S="$fps_s" FRAMES_N="$frames" "$TOOLS_PY" "$BENCH_TOOLS/clip.py" duration-from-frames)
        else
            dur=0
        fi
    fi

    echo "$mtime $dur" > "$cache"
    echo "$dur"
}

prepare_clip() {
    compute_clip_start
    local meta_key
    meta_key=$(clip_meta_key)
    if [[ ! -f "$CLIP_META" ]] || [[ "$(cat "$CLIP_META")" != "$meta_key" ]] \
        || [[ ! -f "$CLIP_MP4" ]] || [[ ! -f "$REF_YUV_RAM" ]] \
        || [[ ! -f "$REF_NV12_RAM" ]]; then
        echo "[prep] 从 ${CLIP_START}s 截取 ${CLIP_SEC}s（offset=${CLIP_OFFSET}）→ ${RAMDISK_DIR} ..."
        rm -f "$CLIP_MP4" "$REF_Y4M_RAM" "$REF_YUV_RAM" "$REF_NV12_RAM"
        echo "$meta_key" > "$CLIP_META"

        local demux_ff demux_probe
        local -a prep_in
        demux_ff=$(resolve_demux_ffmpeg)
        demux_probe=$(resolve_demux_ffprobe)
        prep_in=()
        read -ra prep_in <<< "$(prep_input_format_args)"

        if src_is_raw_elementary; then
            echo "[prep] 裸码流输入，使用 ${demux_ff} ${prep_in[*]} 解码 ..."
            # 裸码流须在 -i 之后 -ss（input seek 不可靠）。
            "$demux_ff" -y "${prep_in[@]}" -i "$SRC_VIDEO" -ss "$CLIP_START" -t "$CLIP_SEC" \
                -pix_fmt yuv420p -f yuv4mpegpipe "$REF_Y4M_RAM" 2>/dev/null
            "$demux_ff" -y "${prep_in[@]}" -i "$SRC_VIDEO" -ss "$CLIP_START" -t "$CLIP_SEC" \
                -pix_fmt yuv420p -f rawvideo "$REF_YUV_RAM" 2>/dev/null
            "$demux_ff" -y "${prep_in[@]}" -i "$SRC_VIDEO" -ss "$CLIP_START" -t "$CLIP_SEC" \
                -pix_fmt nv12 -f rawvideo "$REF_NV12_RAM" 2>/dev/null

            local _w _h _fps
            _w=$("$demux_probe" -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$REF_Y4M_RAM")
            _h=$("$demux_probe" -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$REF_Y4M_RAM")
            _fps=$("$demux_probe" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$REF_Y4M_RAM")
            # clip.mp4 仅供 ffprobe / rkvc_transcode；由已解码 YUV 封装，避免二次有损。
            "$demux_ff" -y -f rawvideo -pix_fmt yuv420p -video_size "${_w}x${_h}" \
                -framerate "$_fps" -i "$REF_YUV_RAM" -c:v "$PREP_ELEM_ENCODER" \
                -rc_mode "$PREP_ELEM_RC_MODE" -qp_init "$PREP_ELEM_QP" -g "$PREP_ELEM_GOP" -an \
                "$CLIP_MP4" 2>/dev/null
        else
            # 先解码到 REF，再从 YUV 重封装 clip.mp4。
            # 禁止 -c copy：中段切片会带负 PTS / 开 GOP 冗余包，rkmpp 硬解会 drop/dup，
            # 且与 REF 帧数不一致，导致 rkvc_transcode RD 测质错位。
            "$PREP_FFMPEG" -y -ss "$CLIP_START" -t "$CLIP_SEC" -i "$SRC_VIDEO" \
                -an -pix_fmt yuv420p -f yuv4mpegpipe "$REF_Y4M_RAM" 2>/dev/null
            "$PREP_FFMPEG" -y -ss "$CLIP_START" -t "$CLIP_SEC" -i "$SRC_VIDEO" \
                -an -pix_fmt yuv420p -f rawvideo "$REF_YUV_RAM" 2>/dev/null
            "$PREP_FFMPEG" -y -ss "$CLIP_START" -t "$CLIP_SEC" -i "$SRC_VIDEO" \
                -an -pix_fmt nv12 -f rawvideo "$REF_NV12_RAM" 2>/dev/null

            local _w _h _fps
            _w=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$SRC_VIDEO")
            _h=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$SRC_VIDEO")
            _fps=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$SRC_VIDEO")
            "$PREP_FFMPEG" -y -f rawvideo -pix_fmt yuv420p -video_size "${_w}x${_h}" \
                -framerate "$_fps" -i "$REF_YUV_RAM" -c:v "$PREP_ELEM_ENCODER" \
                -rc_mode "$PREP_ELEM_RC_MODE" -qp_init "$PREP_ELEM_QP" -g "$PREP_ELEM_GOP" -an \
                "$CLIP_MP4" 2>/dev/null
        fi
    fi
}

prepare_clip
sync_ref_to_ram

SRC_CLIP="$CLIP_MP4"
WIDTH=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$SRC_CLIP")
HEIGHT=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$SRC_CLIP")
FPS_NUM=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$SRC_CLIP" | awk -F/ '{print $1}')
FPS_DEN=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$SRC_CLIP" | awk -F/ '{if (NF>1) print $2; else print 1}')
FRAMES=$("$FFPROBE" -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$SRC_CLIP")
DURATION=$("$FFPROBE" -v error -show_entries format=duration -of csv=p=0 "$SRC_CLIP")

if [[ ! -f "$CSV" ]]; then
    echo "$CSV_HEADER" > "$CSV"
elif ! head -1 "$CSV" | grep -q rga_sec; then
    "$TOOLS_PY" "$BENCH_TOOLS/csv_io.py" migrate "$CSV"
fi

# 合并分片 CSV → rd_data.csv。
# session（默认）：仅写入本次 results_*.csv，rd_data.csv 与本次 RUN_CODECS 一致。
# accumulate：未出现在分片中的 codec 保留旧 rd_data.csv 行。
finalize_csv() {
    "$TOOLS_PY" "$BENCH_TOOLS/csv_io.py" finalize \
        "$CSV" "$WORKDIR" "$BENCH_CSV_MODE" "$SESSION_META" "$SESSION_CODECS"
}

write_bench_session_meta() {
    cat >"$SESSION_META" <<EOF
run_started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
csv_mode=$BENCH_CSV_MODE
run_codecs=$RUN_CODECS
enc_scale_denom=$ENC_SCALE_DENOM
upscale_algos=$UPSCALE_ALGOS
target_kbps=$TARGET_KBPS
clip_sec=$CLIP_SEC
clip_start=$CLIP_START
frames=$FRAMES
width=$WIDTH
height=$HEIGHT
EOF
}

parse_quality_log() {
    "$TOOLS_PY" "$BENCH_TOOLS/quality.py" parse "$1"
}

measure_quality() {
    local decoded="$1" stats="$2"
    "$PREP_FFMPEG" -y -i "$SRC_CLIP" -i "$decoded" -shortest \
        -lavfi "[0:v][1:v]psnr=stats_file=${stats}.psnr;[0:v][1:v]ssim=stats_file=${stats}.ssim" \
        -f null /dev/null 2>"${stats}.log"
    parse_quality_log "${stats}.log"
}

measure_quality_yuv() {
    local decoded_yuv="$1" ref_yuv="$2" frames="$3" stats="$4"
    local fps_num="$5" fps_den="$6"
    local fps=$((fps_num / fps_den))
    "$PREP_FFMPEG" -y -f rawvideo -pix_fmt yuv420p -s "${WIDTH}x${HEIGHT}" -r "$fps" -i "$decoded_yuv" \
        -f rawvideo -pix_fmt yuv420p -s "${WIDTH}x${HEIGHT}" -r "$fps" -i "$ref_yuv" \
        -frames:v "$frames" \
        -lavfi "[0:v][1:v]psnr=stats_file=${stats}.psnr;[0:v][1:v]ssim=stats_file=${stats}.ssim" \
        -f null /dev/null 2>"${stats}.log"
    parse_quality_log "${stats}.log"
}

measure_quality_nv12() {
    local decoded_nv12="$1" ref_nv12="$2" frames="$3" stats="$4"
    local fps_num="$5" fps_den="$6"
    local fps=$((fps_num / fps_den))
    "$PREP_FFMPEG" -y -f rawvideo -pix_fmt nv12 -s "${WIDTH}x${HEIGHT}" -r "$fps" -i "$decoded_nv12" \
        -f rawvideo -pix_fmt nv12 -s "${WIDTH}x${HEIGHT}" -r "$fps" -i "$ref_nv12" \
        -frames:v "$frames" \
        -lavfi "[0:v][1:v]psnr=stats_file=${stats}.psnr;[0:v][1:v]ssim=stats_file=${stats}.ssim" \
        -f null /dev/null 2>"${stats}.log"
    parse_quality_log "${stats}.log"
}

actual_kbps() {
    local file="$1" frames="${2:-}" dur="${3:-$DURATION}"
    "$TOOLS_PY" "$BENCH_TOOLS/bitrate.py" "$file" "$frames" "$dur" "$FPS_NUM" "$FPS_DEN"
}

# 写入当前 $CSV（本次路线的分片文件，非 rd_data.csv 追加）。
write_csv_row() {
    "$TOOLS_PY" "$BENCH_TOOLS/csv_io.py" write-row "$@"
}

write_csv_row_session() {
    "$TOOLS_PY" "$BENCH_TOOLS/csv_io.py" write-session-row "$@"
}

bench_cal() {
    "$TOOLS_PY" "$BENCH_TOOLS/config.py" lookup "$BENCH_CONFIG" "$PROJECT_ROOT" "$1" "$2"
}

rkmpp_cqp_encode_args() {
    local qp="$1"
    printf '%s\n' -rc_mode "$RKMPP_RC_MODE" -qp_init "$qp"
}

svt_vbr_args() {
    local kbps="$1"
    printf '%s\n' --rc 1 --tbr "$kbps"
}

svt_full_encode_args() {
    local kbps="$1"
    if [[ "$SVT_RD_MODE" == "vbr" ]]; then
        svt_vbr_args "$kbps"
        return
    fi
    local crf
    crf=$(bench_cal "svt_av1.full_crf" "$kbps")
    printf '%s\n' --rc 0 --crf "$crf"
}

svt_lo_encode_args() {
    local kbps="$1"
    if [[ "$SVT_RD_MODE" == "vbr" ]]; then
        svt_vbr_args "$kbps"
        return
    fi
    local qp
    qp=$(bench_cal "svt_av1.low_qp" "$kbps")
    printf '%s\n' --rc 0 --aq-mode 0 --qp "$qp"
}

svt_superres_codec_name() {
    echo "svt-av1+superres"
}

svt_superres_mode_label() {
    case "${SVT_SUPERRES_MODE:-4}" in
        1) echo "fixed/${SVT_SUPERRES_DENOM}" ;;
        3) echo "qthresh" ;;
        4) echo "auto" ;;
        *) echo "mode${SVT_SUPERRES_MODE}" ;;
    esac
}

svt_superres_cli_args() {
    printf '%s\n' \
        --superres-mode "$SVT_SUPERRES_MODE" \
        --superres-denom "$SVT_SUPERRES_DENOM" \
        --superres-kf-denom "$SVT_SUPERRES_KF_DENOM" \
        --superres-qthres "$SVT_SUPERRES_QTHRES" \
        --superres-kf-qthres "$SVT_SUPERRES_KF_QTHRES"
}

svt_superres_encode_args() {
    local kbps="$1"
    local -a base superres
    mapfile -t base < <(svt_full_encode_args "$kbps")
    mapfile -t superres < <(svt_superres_cli_args)
    printf '%s\n' "${base[@]}" "${superres[@]}"
}

h264_qp_for_target() { bench_cal "h264.full" "$1"; }
h265_qp_for_target() { bench_cal "h265.full" "$1"; }
h264_lo_qp_for_target() { bench_cal "h264.low" "$1"; }
h265_lo_qp_for_target() { bench_cal "h265.low" "$1"; }

sync_enc_ref_to_ram() {
    enc_dims
    REF_Y4M_ENC="$RAMDISK_DIR/clip_enc_${ENC_SCALE_DENOM}x_${ENC_W}x${ENC_H}.y4m"
    REF_NV12_ENC="$RAMDISK_DIR/clip_enc_${ENC_SCALE_DENOM}x_${ENC_W}x${ENC_H}.nv12"
    local enc_meta="$RAMDISK_DIR/clip_enc_${ENC_SCALE_DENOM}x.meta"
    local enc_method="$PREP_DOWNSCALE_METHOD"

    if [[ ! -f "$enc_meta" ]] || [[ "$(cat "$enc_meta")" != "$enc_method" ]] \
        || [[ ! -f "$REF_NV12_ENC" ]] || [[ "$REF_NV12_RAM" -nt "$REF_NV12_ENC" ]]; then
        echo "[prep] RGA 下采样参考帧 ${WIDTH}x${HEIGHT} → ${ENC_W}x${ENC_H} (1/${ENC_SCALE_DENOM}, ${PREP_DOWNSCALE_ALGO}, NV12) ..."
        downscale_nv12 "$REF_NV12_RAM" "$REF_NV12_ENC" "$FRAMES" || return 1
        echo "$enc_method" > "$enc_meta"
        rm -f "$REF_Y4M_ENC"
    fi
    if [[ ! -f "$REF_Y4M_ENC" ]] || [[ "$REF_NV12_ENC" -nt "$REF_Y4M_ENC" ]]; then
        "$FFMPEG" -y -f rawvideo -pix_fmt nv12 -video_size "${ENC_W}x${ENC_H}" \
            -framerate "$FPS_NUM/$FPS_DEN" -i "$REF_NV12_ENC" \
            -pix_fmt yuv420p -f yuv4mpegpipe "$REF_Y4M_ENC" 2>/dev/null
    fi
}

# IVF/AV1 等 drm_prime 输出需 hwdownload；MP4 走 RKMPP 硬解 + hwdownload。
ffmpeg_rkmpp_decoder_for_file() {
    local file="$1"
    local name
    name=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=codec_name \
        -of csv=p=0 "$file" 2>/dev/null || true)
    rkvc_rkmpp_decoder "$name"
}

ffmpeg_to_nv12_raw() {
    local input="$1" out_nv12="$2" log="$3"
    shift 3
    local dec

    if [[ "${input##*.}" == "ivf" ]]; then
        "$FFMPEG" -y -c:v av1_rkmpp -i "$input" \
            -vf "hwdownload,format=nv12" -pix_fmt nv12 \
            "$@" -f rawvideo "$out_nv12" 2>>"$log"
        return
    fi

    dec=$(ffmpeg_rkmpp_decoder_for_file "$input")
    "$FFMPEG" -y -c:v "$dec" -i "$input" -pix_fmt nv12 \
        "$@" -f rawvideo "$out_nv12" 2>>"$log"
}

ffmpeg_to_yuv420p_raw() {
    local input="$1" out_yuv="$2" log="$3"
    shift 3
    local dec

    if [[ "${input##*.}" == "ivf" ]]; then
        "$FFMPEG" -y -c:v av1_rkmpp -i "$input" \
            -vf "hwdownload,format=nv12" -pix_fmt yuv420p \
            "$@" -f rawvideo "$out_yuv" 2>>"$log"
        return
    fi

    dec=$(ffmpeg_rkmpp_decoder_for_file "$input")
    # passthrough：按包输出，避免错误时间戳触发 CFR drop/dup 导致与 REF 错位。
    "$FFMPEG" -y -c:v "$dec" -i "$input" -fps_mode passthrough -pix_fmt yuv420p \
        "$@" -f rawvideo "$out_yuv" 2>>"$log"
}

# superres 码流：av1_rkmpp hwdownload 会因 stride/width 不一致崩溃，暂用软解。
ffmpeg_superres_to_yuv420p_raw() {
    local input="$1" out_yuv="$2" log="$3"
    shift 3
    local ff="$SVT_SUPERRES_FFMPEG"
    if [[ -z "$ff" || ! -x "$ff" ]]; then
        echo "[error] superres 解码需在 config.json 设置 paths.superres_decode_ffmpeg（项目 ffmpeg 无 libaom）" >&2
        return 1
    fi
    "$ff" -y -c:v libaom-av1 -i "$input" -pix_fmt yuv420p \
        "$@" -f rawvideo "$out_yuv" 2>>"$log"
}


upscale_yuv() {
    local in_yuv="$1" out_yuv="$2" sw="$3" sh="$4" dw="$5" dh="$6" algo="$7" frames="$8"
    if [[ ! -x "$RKVC_YUV_UPSCALE" ]]; then
        echo "[error] RGA 缩放工具未构建: $RKVC_YUV_UPSCALE" >&2
        return 1
    fi
    env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" "$RKVC_YUV_UPSCALE" --in "$in_yuv" --out "$out_yuv" \
        --sw "$sw" --sh "$sh" --dw "$dw" --dh "$dh" \
        --algo "$algo" --pix-fmt yuv420p --frames "$frames" || return 1
}

upscale_nv12() {
    local in_nv12="$1" out_nv12="$2" sw="$3" sh="$4" dw="$5" dh="$6" algo="$7" frames="$8"
    if [[ ! -x "$RKVC_YUV_UPSCALE" ]]; then
        echo "[error] RGA 缩放工具未构建: $RKVC_YUV_UPSCALE" >&2
        return 1
    fi
    env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" "$RKVC_YUV_UPSCALE" --in "$in_nv12" --out "$out_nv12" \
        --sw "$sw" --sh "$sh" --dw "$dw" --dh "$dh" \
        --algo "$algo" --pix-fmt nv12 --frames "$frames" || return 1
}

# Session 硬解 (DMABUF) + RGA 上采样；输出 decode/rga/write/postproc 计时。
session_decode_upscale() {
    local bitstream="$1" out_nv12="$2" algo="$3" log="$4"
    if [[ ! -x "$RKVC_SESSION_UPSCALE" ]]; then
        echo "[error] Session 上采样工具未构建: $RKVC_SESSION_UPSCALE" >&2
        return 1
    fi
    local -a extra=()
    if [[ "$algo" == "rkvc_sr" ]]; then
        if [[ ! -f "$RKVC_SR_MODEL" ]]; then
            echo "[error] RKVC 超分模型未找到: $RKVC_SR_MODEL" >&2
            return 1
        fi
        extra+=(--rkvc-sr-model "$RKVC_SR_MODEL")
    fi
    "$RKVC_SESSION_UPSCALE" -i "$bitstream" -o "$out_nv12" \
        --width "$WIDTH" --height "$HEIGHT" \
        --enc-scale-denom "$ENC_SCALE_DENOM" \
        --post-upscale "$algo" "${extra[@]}" --print-timing 2>"$log"
}

downscale_yuv() {
    local in_yuv="$1" out_yuv="$2" frames="$3"
    enc_dims
    upscale_yuv "$in_yuv" "$out_yuv" "$WIDTH" "$HEIGHT" "$ENC_W" "$ENC_H" "$PREP_DOWNSCALE_ALGO" "$frames"
}

downscale_nv12() {
    local in_nv12="$1" out_nv12="$2" frames="$3"
    enc_dims
    upscale_nv12 "$in_nv12" "$out_nv12" "$WIDTH" "$HEIGHT" "$ENC_W" "$ENC_H" bilinear "$frames"
}

post_upscale_codec_name() {
    local base="$1"
    local algo="$2"
    echo "${base}+up${ENC_SCALE_DENOM}x-${algo}"
}

post_upscale_base_enabled() {
    local base="$1"
    local algo name
    if codec_enabled "post-upscale-${base}"; then
        return 0
    fi
    IFS=',' read -ra _algos <<< "$UPSCALE_ALGOS"
    for algo in "${_algos[@]}"; do
        name=$(post_upscale_codec_name "$base" "$algo")
        codec_enabled "$name" && return 0
    done
    if codec_enabled post-upscale; then
        if [[ -n "${POST_UPSCALE_BASES:-}" ]]; then
            [[ ",$POST_UPSCALE_BASES," == *",$base,"* ]]
            return
        fi
        codec_enabled "$base" && return 0
    fi
    return 1
}

post_upscale_algo_enabled() {
    local base="$1"
    local algo="$2"
    local name
    name=$(post_upscale_codec_name "$base" "$algo")
    if codec_enabled "$name"; then
        return 0
    fi
    post_upscale_base_enabled "$base" && [[ ",$UPSCALE_ALGOS," == *",$algo,"* ]]
}

post_upscale_will_rerun() {
    local base algo
    for base in "${POST_UPSCALE_BASE_LIST[@]}"; do
        if post_upscale_base_enabled "$base"; then
            return 0
        fi
    done
    return 1
}

svt_preset_for_base() {
    case "$1" in
        svt-av1-hq) echo "${SVT_HQ_PRESET:-4}" ;;
        *)          echo "$SVT_PRESET" ;;
    esac
}

run_h264_hw() {
    local kbps="$1"
    local ramdir logdir
    ramdir=$(bench_ramdir h264 "$kbps")
    logdir=$(bench_logdir h264 "$kbps")
    mkdir -p "$ramdir" "$logdir"
    local qp bitstream="$ramdir/stream.mp4" decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local -a enc_args
    qp=$(h264_qp_for_target "$kbps")
    mapfile -t enc_args < <(rkmpp_cqp_encode_args "$qp")
    echo "[run] h264 RKMPP @ ${kbps}kbps (CQP qp=${qp}, raw ${WIDTH}x${HEIGHT})"
    local t0 t1 t2 br q
    t0=$(date +%s.%N)
    "$FFMPEG" -y -f rawvideo -pix_fmt yuv420p -video_size "${WIDTH}x${HEIGHT}" \
        -framerate "$FPS_NUM/$FPS_DEN" -i "$REF_YUV_RAM" -c:v h264_rkmpp \
        "${enc_args[@]}" -g "$RKMPP_GOP" -an \
        "$bitstream" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)
    ffmpeg_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log"
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$FRAMES")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$FRAMES" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "h264" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

run_h265_hw() {
    local kbps="$1"
    local ramdir logdir
    ramdir=$(bench_ramdir h265 "$kbps")
    logdir=$(bench_logdir h265 "$kbps")
    mkdir -p "$ramdir" "$logdir"
    local qp bitstream="$ramdir/stream.mp4" decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local -a enc_args
    qp=$(h265_qp_for_target "$kbps")
    mapfile -t enc_args < <(rkmpp_cqp_encode_args "$qp")
    echo "[run] h265 RKMPP @ ${kbps}kbps (CQP qp=${qp}, raw ${WIDTH}x${HEIGHT})"
    local t0 t1 t2 br q
    t0=$(date +%s.%N)
    "$FFMPEG" -y -f rawvideo -pix_fmt yuv420p -video_size "${WIDTH}x${HEIGHT}" \
        -framerate "$FPS_NUM/$FPS_DEN" -i "$REF_YUV_RAM" -c:v hevc_rkmpp \
        "${enc_args[@]}" -g "$RKMPP_GOP" -an \
        "$bitstream" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)
    ffmpeg_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log"
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$FRAMES")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$FRAMES" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "h265" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

run_svt_av1_at_preset() {
    local csv_codec="$1" kbps="$2" preset="$3"
    local ramdir logdir
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    if [[ ! -x "$SVT_ENC" ]]; then
        echo "[skip] SVT-AV1 未构建: $SVT_ENC (运行 ./scripts/build-svt.sh)"
        return 0
    fi
    local bitstream="$ramdir/stream.ivf"
    local decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local -a svt_args
    mapfile -t svt_args < <(svt_full_encode_args "$kbps")
    echo "[run] ${csv_codec} @ ${kbps}kbps (preset ${preset}, mode ${SVT_RD_MODE})"
    local t0 t1 t2 br q
    t0=$(date +%s.%N)
    "$SVT_ENC" --input "$REF_Y4M_RAM" -b "$bitstream" --preset "$preset" \
        "${svt_args[@]}" --keyint "$SVT_KEYINT" --lp "$SVT_LP" -n "$FRAMES" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)
    ffmpeg_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log"
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$FRAMES")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$FRAMES" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

run_svt_av1() {
    run_svt_av1_at_preset svt-av1 "$1" "$SVT_PRESET"
}

# 非实时高质量档：更慢的 SVT preset（默认 hq_preset=4），目标编码速度 ≥1 fps@1080p
run_svt_av1_hq() {
    run_svt_av1_at_preset svt-av1-hq "$1" "${SVT_HQ_PRESET:-4}"
}

run_svt_av1_superres() {
    local kbps="$1"
    local csv_codec
    csv_codec=$(svt_superres_codec_name)
    local ramdir logdir
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    if [[ ! -x "$SVT_ENC" ]]; then
        echo "[skip] SVT-AV1 未构建: $SVT_ENC (运行 ./scripts/build-svt.sh)"
        return 0
    fi
    local bitstream="$ramdir/stream.ivf"
    local decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local -a svt_args
    mapfile -t svt_args < <(svt_superres_encode_args "$kbps")
    echo "[run] ${csv_codec} @ ${kbps}kbps (superres $(svt_superres_mode_label), preset ${SVT_PRESET}, mode ${SVT_RD_MODE})"
    echo "[warn] svt-av1+superres 为实验路线；解码走 ${SVT_SUPERRES_FFMPEG}（须在 config.json 配置）" >&2
    local t0 t1 t2 br q
    t0=$(date +%s.%N)
    "$SVT_ENC" --input "$REF_Y4M_RAM" -b "$bitstream" --preset "$SVT_PRESET" \
        "${svt_args[@]}" --keyint "$SVT_KEYINT" --lp "$SVT_LP" -n "$FRAMES" 2>"$logdir/enc.log" || return 1
    t1=$(date +%s.%N)
    ffmpeg_superres_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log" -frames:v "$FRAMES" || return 1
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$FRAMES")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$FRAMES" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

run_rkmpp_post_upscale() {
    local base="$1" algo="$2" kbps="$3"
    local csv_codec enc dec qp ramdir logdir
    csv_codec=$(post_upscale_codec_name "$base" "$algo")
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    case "$base" in
        h264)
            enc=h264_rkmpp
            dec=h264_rkmpp
            qp=$(h264_lo_qp_for_target "$kbps")
            ;;
        h265)
            enc=hevc_rkmpp
            dec=hevc_rkmpp
            qp=$(h265_lo_qp_for_target "$kbps")
            ;;
        *)
            echo "[error] 不支持的 post-upscale 基线: $base" >&2
            return 1
            ;;
    esac
    enc_dims
    sync_enc_ref_to_ram
    local bitstream="$ramdir/stream.mp4"
    local upscaled_nv12="$ramdir/upscaled.nv12"
    local stats="$logdir/stats"
    local -a enc_args
    mapfile -t enc_args < <(rkmpp_cqp_encode_args "$qp")
    echo "[run] ${csv_codec} @ ${kbps}kbps (encode ${ENC_W}x${ENC_H} CQP qp=${qp}, session decode+upscale=${algo})"
    local t0 t1 timing dec_sec post_sec br q
    t0=$(date +%s.%N)
    "$FFMPEG" -y -f rawvideo -pix_fmt nv12 -video_size "${ENC_W}x${ENC_H}" \
        -framerate "$FPS_NUM/$FPS_DEN" -i "$REF_NV12_ENC" -c:v "$enc" \
        "${enc_args[@]}" -g "$RKMPP_GOP" -an "$bitstream" 2>"$logdir/enc.log" || return 1
    t1=$(date +%s.%N)
    timing=$(session_decode_upscale "$bitstream" "$upscaled_nv12" "$algo" "$logdir/dec.log") || return 1
    dec_sec=$(echo "$timing" | sed -n 's/.*decode_sec=\([0-9.]*\).*/\1/p')
    rga_sec=$(echo "$timing" | sed -n 's/.*rga_sec=\([0-9.]*\).*/\1/p')
    write_sec=$(echo "$timing" | sed -n 's/.*write_sec=\([0-9.]*\).*/\1/p')
    post_sec=$(echo "$timing" | sed -n 's/.*postproc_sec=\([0-9.]*\).*/\1/p')
    br=$(actual_kbps "$bitstream" "$FRAMES")
    q=$(measure_quality_nv12 "$upscaled_nv12" "$REF_NV12_RAM" "$FRAMES" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row_session "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" \
        "$dec_sec" "$rga_sec" "$write_sec" "$post_sec"
}

run_svt_av1_post_upscale() {
    local base="$1" algo="$2" kbps="$3"
    local csv_codec ramdir logdir preset
    csv_codec=$(post_upscale_codec_name "$base" "$algo")
    preset=$(svt_preset_for_base "$base")
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    if [[ ! -x "$SVT_ENC" ]]; then
        echo "[skip] SVT-AV1 未构建: $SVT_ENC (运行 ./scripts/build-svt.sh)"
        return 0
    fi
    enc_dims
    sync_enc_ref_to_ram
    local bitstream="$ramdir/stream.ivf"
    local upscaled_nv12="$ramdir/upscaled.nv12"
    local stats="$logdir/stats"
    echo "[run] ${csv_codec} @ ${kbps}kbps (encode ${ENC_W}x${ENC_H}, session decode+upscale=${algo}, preset ${preset}, mode ${SVT_RD_MODE})"
    local t0 t1 timing dec_sec rga_sec write_sec post_sec br q
    local -a svt_args
    mapfile -t svt_args < <(svt_lo_encode_args "$kbps")
    t0=$(date +%s.%N)
    "$SVT_ENC" --input "$REF_Y4M_ENC" -b "$bitstream" --preset "$preset" \
        "${svt_args[@]}" --keyint "$SVT_KEYINT" --lp "$SVT_LP" -n "$FRAMES" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)
    timing=$(session_decode_upscale "$bitstream" "$upscaled_nv12" "$algo" "$logdir/dec.log") || return 1
    dec_sec=$(echo "$timing" | sed -n 's/.*decode_sec=\([0-9.]*\).*/\1/p')
    rga_sec=$(echo "$timing" | sed -n 's/.*rga_sec=\([0-9.]*\).*/\1/p')
    write_sec=$(echo "$timing" | sed -n 's/.*write_sec=\([0-9.]*\).*/\1/p')
    post_sec=$(echo "$timing" | sed -n 's/.*postproc_sec=\([0-9.]*\).*/\1/p')
    br=$(actual_kbps "$bitstream" "$FRAMES")
    q=$(measure_quality_nv12 "$upscaled_nv12" "$REF_NV12_RAM" "$FRAMES" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row_session "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" \
        "$dec_sec" "$rga_sec" "$write_sec" "$post_sec"
}

rkvc_rkmpp_decoder() {
    case "$1" in
        h264) echo h264_rkmpp ;;
        hevc) echo hevc_rkmpp ;;
        av1)  echo av1_rkmpp ;;
        *)    echo hevc_rkmpp ;;
    esac
}

stream_frame_count() {
    local file="$1"
    local count
    count=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 "$file" 2>/dev/null || echo 0)
    echo "${count:-0}"
}

run_rkvc_transcode_policy() {
    local policy="$1" kbps="$2"
    local csv_codec="rkvc-${policy}"
    local ramdir logdir
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    local bitstream="$ramdir/stream.mp4" decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    if [[ ! -x "$RKVC_TRANS" ]]; then
        echo "[skip] rkvc_transcode 未构建: $RKVC_TRANS"
        return 0
    fi
    local min_frames=$((FRAMES - 2))
    local attempt got t0 t1 t2 br q qp enc_frames
    for attempt in 1 2 3; do
        rm -f "$bitstream" "$decoded_yuv" "$logdir"/*.log "$logdir"/stats.*
        t0=$(date +%s.%N)
        case "$policy" in
            realtime)
                qp=$(h264_qp_for_target "$kbps")
                echo "[run] rkvc session (realtime → H.264, CQP qp=${qp}) @ ${kbps}kbps"
                "$RKVC_TRANS" -i "$SRC_CLIP" -o "$bitstream" -p realtime -b "$((kbps * 1000))" \
                    --rc-mode cqp --qp "$qp" -s "${WIDTH}x${HEIGHT}" 2>"$logdir/enc.log"
                ;;
            balanced)
                qp=$(h265_qp_for_target "$kbps")
                echo "[run] rkvc session (balanced → HEVC, CQP qp=${qp}) @ ${kbps}kbps"
                "$RKVC_TRANS" -i "$SRC_CLIP" -o "$bitstream" -p balanced -b "$((kbps * 1000))" \
                    --rc-mode cqp --qp "$qp" -s "${WIDTH}x${HEIGHT}" 2>"$logdir/enc.log"
                ;;
            *)
                echo "[error] 未知 rkvc policy: $policy" >&2
                return 1
                ;;
        esac
        got=$(stream_frame_count "$bitstream")
        if [[ "$got" -ge "$min_frames" ]]; then
            break
        fi
        echo "[warn] ${csv_codec} @ ${kbps}kbps 仅 ${got}/${FRAMES} 帧，重试..." >&2
    done
    enc_frames=$(stream_frame_count "$bitstream")
    if [[ "$enc_frames" -lt "$min_frames" ]]; then
        echo "[error] ${csv_codec} @ ${kbps}kbps 帧数不足: ${enc_frames}/${FRAMES}" >&2
        return 1
    fi
    t1=$(date +%s.%N)
    ffmpeg_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log"
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$enc_frames")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$enc_frames" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

run_rkvc_quality() {
    local kbps="$1"
    local csv_codec="rkvc-quality"
    local ramdir logdir
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    local bitstream="$ramdir/stream.ivf" decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local fps=$((FPS_NUM / FPS_DEN))
    if [[ ! -x "$RKVC_ENC" ]]; then
        echo "[skip] rkvc_encode 未构建: $RKVC_ENC"
        return 0
    fi
    echo "[run] rkvc session (quality → AV1 SVT p${SVT_PRESET}, lp=${SVT_LP}, YUV encode) @ ${kbps}kbps"
    local t0 t1 t2 br q enc_frames
    local -a svt_session_args=(--svt-lp "${SVT_LP:-4}")
    [[ "${SVT_RTC:-0}" == "1" ]] && svt_session_args+=(--svt-rtc)
    t0=$(date +%s.%N)
    "$RKVC_ENC" -i "$REF_YUV_RAM" -o "$bitstream" -p quality -b "$((kbps * 1000))" \
        --rc-mode vbr --pix-fmt yuv420p -s "${WIDTH}x${HEIGHT}" -r "$fps" \
        "${svt_session_args[@]}" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)
    enc_frames=$(stream_frame_count "$bitstream")
    if [[ "$enc_frames" -lt $((FRAMES - 2)) ]]; then
        echo "[error] ${csv_codec} @ ${kbps}kbps 帧数不足: ${enc_frames}/${FRAMES}" >&2
        return 1
    fi
    ffmpeg_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log" -frames:v "$enc_frames"
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$enc_frames")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$enc_frames" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

# 非实时高质量：Session offline → SVT-AV1 hq_preset（默认 4）
run_rkvc_offline() {
    local kbps="$1"
    local csv_codec="rkvc-offline"
    local ramdir logdir
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    local bitstream="$ramdir/stream.ivf" decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local fps=$((FPS_NUM / FPS_DEN))
    local hq_preset="${SVT_HQ_PRESET:-4}"
    if [[ ! -x "$RKVC_ENC" ]]; then
        echo "[skip] rkvc_encode 未构建: $RKVC_ENC"
        return 0
    fi
    echo "[run] rkvc session (offline → AV1 SVT p${hq_preset}, lp=${SVT_LP}, YUV encode) @ ${kbps}kbps"
    local t0 t1 t2 br q enc_frames
    local -a svt_session_args=(--svt-lp "${SVT_LP:-4}")
    [[ "${SVT_RTC:-0}" == "1" ]] && svt_session_args+=(--svt-rtc)
    t0=$(date +%s.%N)
    "$RKVC_ENC" -i "$REF_YUV_RAM" -o "$bitstream" -p offline -b "$((kbps * 1000))" \
        --rc-mode vbr --pix-fmt yuv420p -s "${WIDTH}x${HEIGHT}" -r "$fps" \
        "${svt_session_args[@]}" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)
    enc_frames=$(stream_frame_count "$bitstream")
    if [[ "$enc_frames" -lt $((FRAMES - 2)) ]]; then
        echo "[error] ${csv_codec} @ ${kbps}kbps 帧数不足: ${enc_frames}/${FRAMES}" >&2
        return 1
    fi
    ffmpeg_to_yuv420p_raw "$bitstream" "$decoded_yuv" "$logdir/dec.log" -frames:v "$enc_frames"
    t2=$(date +%s.%N)
    br=$(actual_kbps "$bitstream" "$enc_frames")
    q=$(measure_quality_yuv "$decoded_yuv" "$REF_YUV_RAM" "$enc_frames" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

# MLVC 神经编解码（-p neural）：与 H.264/HEVC/AV1 平行的一等编解码器。
# 编码 video → .mlvc（rkvc_transcode -p neural），解码 .mlvc → .yuv（rkvc_transcode）。
# 与其他 rkvc policy 不同：MLVC 用 qp 而非码率，固定分辨率，不能被 ffmpeg 解码。
run_rkvc_neural() {
    local kbps="$1"
    local csv_codec="rkvc-neural"
    local ramdir logdir
    ramdir=$(bench_ramdir "$csv_codec" "$kbps")
    logdir=$(bench_logdir "$csv_codec" "$kbps")
    mkdir -p "$ramdir" "$logdir"
    local bitstream="$ramdir/stream.mlvc" decoded_yuv="$ramdir/decoded.yuv" stats="$logdir/stats"
    local enc_clip="$ramdir/clip_${MLVC_W}x${MLVC_H}.mp4"
    local ref_yuv="$ramdir/ref_${MLVC_W}x${MLVC_H}.yuv"
    if [[ ! -x "$RKVC_TRANS" ]]; then
        echo "[skip] rkvc_transcode 未构建: $RKVC_TRANS"
        return 0
    fi
    if [[ ! -f "$MLVC_ENC_MODEL" || ! -f "$MLVC_DEC_MODEL" ]]; then
        echo "[skip] MLVC 模型缺失: $MLVC_ENC_MODEL / $MLVC_DEC_MODEL"
        return 0
    fi
    if [[ ! -f "$MLVC_GAUSSIAN_PMF" || ! -f "$MLVC_BITEST_PMF" ]]; then
        echo "[skip] MLVC PMF 表缺失: $MLVC_GAUSSIAN_PMF / $MLVC_BITEST_PMF"
        return 0
    fi

    # 缩放到 MLVC 模型固定分辨率（640x368），并导出参考 NV12（匹配 MLVC 解码输出）
    "$PREP_FFMPEG" -y -i "$SRC_CLIP" -vf "scale=${MLVC_W}:${MLVC_H}"         -c:v "$PREP_ELEM_ENCODER" -an "$enc_clip" 2>"$logdir/scale.log"
    "$PREP_FFMPEG" -y -i "$enc_clip" -pix_fmt nv12 -f rawvideo "$ref_yuv" 2>"$logdir/ref.log"
    local mlvc_frames
    mlvc_frames=$("$FFPROBE" -v error -count_frames -select_streams v:0         -show_entries stream=nb_read_frames -of csv=p=0 "$enc_clip" 2>/dev/null || echo 0)
    if [[ "$mlvc_frames" -lt $((FRAMES - 2)) ]]; then
        echo "[error] ${csv_codec} 缩放后帧数不足: ${mlvc_frames}/${FRAMES}" >&2
        return 1
    fi

    local qp="${MLVC_QP:-21}"
    echo "[run] rkvc neural (MLVC, qp=${qp}) @ ${kbps}kbps target (${MLVC_W}x${MLVC_H})"

    local t0 t1 t2 br q
    t0=$(date +%s.%N)
    "$RKVC_TRANS" -i "$enc_clip" -o "$bitstream" -p neural         --mlvc-enc "$MLVC_ENC_MODEL"         --mlvc-gaussian-pmf "$MLVC_GAUSSIAN_PMF"         --mlvc-bitest-pmf "$MLVC_BITEST_PMF"         --mlvc-qp "$qp" 2>"$logdir/enc.log"
    t1=$(date +%s.%N)

    # MLVC 码流只能由 rkvc_transcode 解码
    "$RKVC_TRANS" -i "$bitstream" -o "$decoded_yuv"         --mlvc-dec "$MLVC_DEC_MODEL"         --mlvc-gaussian-pmf "$MLVC_GAUSSIAN_PMF"         --mlvc-bitest-pmf "$MLVC_BITEST_PMF" 2>"$logdir/dec.log"
    t2=$(date +%s.%N)

    br=$(actual_kbps "$bitstream" "$mlvc_frames")
    q=$(measure_quality_nv12 "$decoded_yuv" "$ref_yuv" "$mlvc_frames" "$stats" "$FPS_NUM" "$FPS_DEN")
    bench_cleanup_ramdir "$ramdir"
    write_csv_row "$CSV" "$csv_codec" "$kbps" "$br" "$q" "$t0" "$t1" "$t2"
}

run_rkvc_realtime() { run_rkvc_transcode_policy realtime "$1"; }
run_rkvc_balanced()  { run_rkvc_transcode_policy balanced "$1"; }

bench_codec() {
    local fn="$1"
    local csv_part="$WORKDIR/results_${fn}.csv"
    echo "$CSV_HEADER" > "$csv_part"
    local orig_csv="$CSV"
    CSV="$csv_part"
    for kbps in "${BITRATES[@]}"; do
        "$fn" "$kbps"
    done
    CSV="$orig_csv"
}

codec_enabled() {
    [[ ",$RUN_CODECS," == *",$1,"* ]]
}

superres_enabled() {
    [[ "$SVT_SUPERRES_ENABLED" == "1" ]] || \
        codec_enabled svt-av1+superres || codec_enabled svt-av1-superres
}

rkvc_policy_enabled() {
    [[ ",$RKVC_POLICIES," == *",$1,"* ]]
}

rkvc_policy_selected() {
    local policy="$1"
    local csv_codec="rkvc-${policy}"
    if codec_enabled "$csv_codec"; then
        return 0
    fi
    codec_enabled rkvc && rkvc_policy_enabled "$policy"
}

codec_will_rerun() {
    local codec="$1"
    if codec_enabled "$codec"; then
        return 0
    fi
    case "$codec" in
        rkvc|rkvc-realtime|rkvc-balanced|rkvc-quality|rkvc-offline|rkvc-neural)
            codec_enabled rkvc && return 0
            ;;
    esac
    if [[ "$codec" == post-upscale ]]; then
        post_upscale_will_rerun && return 0
    fi
    if [[ "$codec" =~ ^${POST_UPSCALE_BASE_RE}\+up[0-9]+x-(nearest|bilinear|bicubic|rkvc_sr)$ ]]; then
        post_upscale_algo_enabled "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" && return 0
    fi
    if [[ "$codec" =~ ^${POST_UPSCALE_BASE_RE}\+up[0-9]+x$ ]]; then
        post_upscale_base_enabled "${BASH_REMATCH[1]}" && return 0
    fi
    return 1
}

echo "[info] project: $PROJECT_ROOT"
echo "[info] config:  $BENCH_CONFIG"
echo "[info] ffmpeg:  $FFMPEG"
echo "[info] clip: ${CLIP_SEC}s @ ${CLIP_START}s (${FRAMES} frames, ${WIDTH}x${HEIGHT}, offset=${CLIP_OFFSET})"
echo "[info] bitrates: ${BITRATES[*]} kbps (TARGET_KBPS)"
echo "[info] svt rd mode: $SVT_RD_MODE (calibrated=CRF/CQP, vbr=--rc 1 --tbr)"
echo "[info] svt preset: $SVT_PRESET  hq_preset: ${SVT_HQ_PRESET:-4}"
if superres_enabled; then
    echo "[info] svt superres: mode=${SVT_SUPERRES_MODE} denom=${SVT_SUPERRES_DENOM} qthres=${SVT_SUPERRES_QTHRES}"
fi
echo "[info] codecs: $RUN_CODECS"
echo "[info] csv mode: $BENCH_CSV_MODE (session=仅本次, accumulate=保留未重跑历史)"
echo "[info] enc scale denom: $ENC_SCALE_DENOM  upscale algos: $UPSCALE_ALGOS"
echo "[info] rkvc policies: $RKVC_POLICIES"
echo "[info] rkvc: $RKVC_TRANS"
if codec_enabled rkvc-neural || { codec_enabled rkvc && rkvc_policy_enabled neural; }; then
    echo "[info] mlvc: ${MLVC_W}x${MLVC_H} qp=${MLVC_QP:-21} enc=${MLVC_ENC_MODEL} dec=${MLVC_DEC_MODEL}"
fi
echo "[info] ramdisk: $RAMDISK_DIR (码流/YUV 中间文件 tmpfs)"

for codec in "${ALL_KNOWN_CODECS[@]}"; do
    codec_will_rerun "$codec" && rm -rf "$RAM_WORK_DIR/$codec"
done
if post_upscale_will_rerun; then
    IFS=',' read -ra _up_algos <<< "$UPSCALE_ALGOS"
    for base in "${POST_UPSCALE_BASE_LIST[@]}"; do
        post_upscale_base_enabled "$base" || continue
        for algo in "${_up_algos[@]}"; do
            _c=$(post_upscale_codec_name "$base" "$algo")
            codec_will_rerun "$_c" && rm -rf "$RAM_WORK_DIR/$_c"
        done
    done
    sync_enc_ref_to_ram
fi
sync_ref_to_ram
write_bench_session_meta
rm -f "$WORKDIR"/results_*.csv

bench_rkvc_policies() {
    local policy fn
    for policy in "${RKVC_ALL_POLICIES[@]}"; do
        if ! rkvc_policy_selected "$policy"; then
            continue
        fi
        fn="run_rkvc_${policy}"
        if [[ "$BENCH_PARALLEL" == "1" ]]; then
            bench_codec "$fn" &
            pids+=($!)
        else
            bench_codec "$fn"
        fi
    done
}

bench_post_upscale_combo() {
    local base="$1" algo="$2"
    local csv_part="$WORKDIR/results_post_${base}_${algo}.csv"
    echo "$CSV_HEADER" > "$csv_part"
    local orig_csv="$CSV"
    CSV="$csv_part"
    for kbps in "${BITRATES[@]}"; do
        case "$base" in
            h264|h265) run_rkmpp_post_upscale "$base" "$algo" "$kbps" ;;
            svt-av1|svt-av1-hq) run_svt_av1_post_upscale "$base" "$algo" "$kbps" ;;
        esac
    done
    CSV="$orig_csv"
}

bench_post_upscale() {
    local base algo c
    declare -A _ran=()
    IFS=',' read -ra _bench_algos <<< "$UPSCALE_ALGOS"
    for base in "${POST_UPSCALE_BASE_LIST[@]}"; do
        post_upscale_base_enabled "$base" || continue
        for algo in "${_bench_algos[@]}"; do
            if ! post_upscale_algo_enabled "$base" "$algo"; then
                continue
            fi
            _ran["${base}|${algo}"]=1
            if [[ "$BENCH_PARALLEL" == "1" ]]; then
                bench_post_upscale_combo "$base" "$algo" &
                pids+=($!)
            else
                bench_post_upscale_combo "$base" "$algo"
            fi
        done
    done
    IFS=',' read -ra _run_codecs <<< "$RUN_CODECS"
    for c in "${_run_codecs[@]}"; do
        if [[ "$c" =~ ^${POST_UPSCALE_BASE_RE}\+up[0-9]+x-(nearest|bilinear|bicubic|rkvc_sr)$ ]]; then
            base="${BASH_REMATCH[1]}"
            algo="${BASH_REMATCH[2]}"
            [[ -n "${_ran[${base}|${algo}]:-}" ]] && continue
            post_upscale_algo_enabled "$base" "$algo" || continue
            if [[ "$BENCH_PARALLEL" == "1" ]]; then
                bench_post_upscale_combo "$base" "$algo" &
                pids+=($!)
            else
                bench_post_upscale_combo "$base" "$algo"
            fi
        fi
    done
}

BENCH_PARALLEL="${BENCH_PARALLEL:-0}"

if [[ "$BENCH_PARALLEL" == "1" ]]; then
    pids=()
    codec_enabled h264 && { bench_codec run_h264_hw & pids+=($!); }
    codec_enabled h265 && { bench_codec run_h265_hw & pids+=($!); }
    codec_enabled svt-av1 && { bench_codec run_svt_av1 & pids+=($!); }
    codec_enabled svt-av1-hq && { bench_codec run_svt_av1_hq & pids+=($!); }
    superres_enabled && { bench_codec run_svt_av1_superres & pids+=($!); }
    bench_rkvc_policies
    bench_post_upscale
    for pid in "${pids[@]}"; do wait "$pid"; done
else
    codec_enabled h264 && bench_codec run_h264_hw
    codec_enabled h265 && bench_codec run_h265_hw
    codec_enabled svt-av1 && bench_codec run_svt_av1
    codec_enabled svt-av1-hq && bench_codec run_svt_av1_hq
    superres_enabled && bench_codec run_svt_av1_superres
    bench_rkvc_policies
    bench_post_upscale
fi

finalize_csv

echo "[done] wrote $CSV"
plot_results "$FRAMES" 2>&1 | tee -a "$RESULTS/benchmark.log"
