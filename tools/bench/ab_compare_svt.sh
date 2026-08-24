#!/usr/bin/env bash
# tools/bench/ab_compare_svt.sh — SVT-AV1 双版本 A/B 对比（同片段、同 CRF 阶梯）
#
# 编码：两个 SvtAv1EncApp 安装前缀同 CRF 编码；
# 解码/测质：项目 ffmpeg 的 av1_rkmpp 硬解 + hwdownload（与 run_rd_benchmark.sh
#   的 ffmpeg_to_yuv420p_raw() 走同一条 RD 测质路径，绝对 PSNR/SSIM 与项目一致）。
# 再算 BD-rate。4.1.0→4.2.0 升级时用本脚本得到 tools/bench/results/svt_ab_compare.csv。
#
# 重跑需先构建两个版本到不同前缀（默认 A=.build/deps/svt-av1-install，
#   B=.build/deps/svt-av1-install-420）；可用环境变量覆盖：
#   SVT_A_BIN / SVT_A_LIB / SVT_A_LABEL   SVT_B_BIN / SVT_B_LIB / SVT_B_LABEL
#   FFMPEG（默认项目 ffmpeg；测质解码路径需 av1_rkmpp）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PY="$ROOT/.venv/bin/python"
if [[ ! -x "$PY" ]]; then
  echo "[error] 共享 Python 环境不存在: $PY" >&2
  echo "请先在仓库根目录运行: uv sync" >&2
  exit 1
fi
TOOLS="$ROOT/tools/bench/tools"; RAM=/dev/shm/rkvc-ab
REF_Y4M="$RAM/ref.y4m"; REF_YUV="$RAM/ref.yuv"
W=1920; H=1080; FRAMES=96; KEYINT=60; LP=4; FPSR=24

FF="${FFMPEG:-$ROOT/third_party/ffmpeg-rockchip/ffmpeg}"
# ffmpeg 经 libavcodec 传递依赖 libSvtAv1Enc.so.4，故解码/测质时需把 SVT lib 入栈。
# 测质用 av1_rkmpp 硬解，需 mpp + ffmpeg 各 lib 子目录 + 项目 SVT lib。
FFLIBS="$(ls -d "$ROOT"/third_party/ffmpeg-rockchip/lib*/ 2>/dev/null | tr '\n' ':')"
MPP_LIB="$ROOT/.build/deps/mpp-install/lib"
SVT_DEFAULT_LIB="$ROOT/.build/deps/svt-av1-install/lib"
FF_LDP="$MPP_LIB:$FFLIBS:$SVT_DEFAULT_LIB"

SVT_A_BIN="${SVT_A_BIN:-$ROOT/.build/deps/svt-av1-install/bin/SvtAv1EncApp}"
SVT_A_LIB="${SVT_A_LIB:-$ROOT/.build/deps/svt-av1-install/lib}"
SVT_A_LABEL="${SVT_A_LABEL:-v410}"
SVT_B_BIN="${SVT_B_BIN:-$ROOT/.build/deps/svt-av1-install-420/bin/SvtAv1EncApp}"
SVT_B_LIB="${SVT_B_LIB:-$ROOT/.build/deps/svt-av1-install-420/lib}"
SVT_B_LABEL="${SVT_B_LABEL:-v420}"

P11_CRFS=(41 46 52 57 62 65 68 70)
P4_CRFS=(45 52 57 62 68)
OUT="$ROOT/tools/bench/results/svt_ab_compare.csv"
printf 'version,preset,crf,actual_kbps,psnr_y,psnr_avg,ssim,encode_sec,enc_fps\n' > "$OUT"

# 测质：av1_rkmpp 硬解 IVF → hwdownload → yuv420p raw，再与 REF 比 PSNR/SSIM。
# 与 run_rd_benchmark.sh 的 ffmpeg_to_yuv420p_raw()（IVF 分支）同一管线。
measure_quality() {  # ivf_in  decoded_yuv  stats_base
  "$FF" -hide_banner -y -c:v av1_rkmpp -i "$1" \
    -vf "hwdownload,format=nv12" -pix_fmt yuv420p \
    -f rawvideo "$2" 2>/dev/null
  "$FF" -hide_banner -y -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r $FPSR -i "$2" \
    -f rawvideo -pix_fmt yuv420p -s ${W}x${H} -r $FPSR -i "$REF_YUV" -frames:v $FRAMES \
    -lavfi "[0:v][1:v]psnr=stats_file=${3}.psnr;[0:v][1:v]ssim=stats_file=${3}.ssim" \
    -f null /dev/null 2>/dev/null
}

run_point() {
  local label="$1" bin="$2" libdir="$3" preset="$4" crf="$5"
  local bs="$RAM/${label}_p${preset}_crf${crf}.ivf"
  local dec="$RAM/${label}_p${preset}_crf${crf}.yuv"
  local stats="$RAM/st_${label}_${preset}_${crf}"
  local t0 t1; t0=$(date +%s.%N)
  LD_LIBRARY_PATH="$libdir" "$bin" --input "$REF_Y4M" -b "$bs" --preset "$preset" \
    --rc 0 --crf "$crf" --keyint "$KEYINT" --lp "$LP" -n "$FRAMES" \
    >/dev/null 2>"$RAM/enc_${label}_${preset}_${crf}.log"
  t1=$(date +%s.%N)
  local enc_sec enc_fps
  enc_sec=$(awk "BEGIN{printf \"%.3f\", $t1-$t0}")
  enc_fps=$(awk "BEGIN{printf \"%.2f\", $FRAMES/($t1-$t0)}")
  export LD_LIBRARY_PATH="$FF_LDP"
  measure_quality "$bs" "$dec" "$stats"
  local br q psny psnu psnv psnavg ssim
  br=$("$PY" "$TOOLS/bitrate.py" "$bs" "$FRAMES" "4" "24" "1")
  q=$("$PY" "$TOOLS/quality.py" parse "$stats")
  IFS=',' read -r psny psnu psnv psnavg ssim <<< "$q"
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$label" "$preset" "$crf" "$br" "$psny" "$psnavg" "$ssim" "$enc_sec" "$enc_fps" >> "$OUT"
  printf '  %-5s p%-2s crf%-3s -> %7.1fkbps PSNR_y=%.2f SSIM=%.4f %.2fs(%.1ffps)\n' \
    "$label" "$preset" "$crf" "$br" "$psny" "$ssim" "$enc_sec" "$enc_fps"
  find "$RAM" -maxdepth 1 -name "${label}_p${preset}_crf${crf}.*" -delete
}

echo "=== preset 11 (quality tier) ==="
for crf in "${P11_CRFS[@]}"; do
  run_point "$SVT_A_LABEL" "$SVT_A_BIN" "$SVT_A_LIB" 11 "$crf"
  run_point "$SVT_B_LABEL" "$SVT_B_BIN" "$SVT_B_LIB" 11 "$crf"
done
echo "=== preset 4 (offline/HQ tier) ==="
for crf in "${P4_CRFS[@]}"; do
  run_point "$SVT_A_LABEL" "$SVT_A_BIN" "$SVT_A_LIB" 4 "$crf"
  run_point "$SVT_B_LABEL" "$SVT_B_BIN" "$SVT_B_LIB" 4 "$crf"
done
echo "[done] $OUT"
