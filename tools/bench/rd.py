#!/usr/bin/env python3
"""Board-side RD measurements. stdlib only; ffmpeg is used for pixel metrics."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time

try:
    from .benchmark import system_metadata
except ImportError:
    from benchmark import system_metadata


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def model_listing(stdout):
    # 有些厂商库会在 CLI JSON 之前向 stdout 打印启动横幅。
    candidates = []
    for line in stdout.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and isinstance(value.get('models'), list):
            candidates.append(value['models'])
    if len(candidates) != 1:
        raise ValueError('expected one model registry JSON object in CLI output')
    return candidates[0]


def progress(message):
    try:
        print(message, flush=True)
    except BrokenPipeError:
        # 丢失进度消费者不能取消长达数小时的测量。
        sys.stdout = open(os.devnull, 'w', encoding='utf-8')


def point_key(row):
    return tuple(row.get(k) for k in ('sequence', 'codec', 'branch', 'qp'))


DEFAULT_RECON = ('low_native', 'bicubic', 'lanczos', 'sr')
MLVC_CODECS = ('mlvc', 'mlvc-s')


def planned_branches(config, codec):
    if codec in MLVC_CODECS or not config.get('native_reference', True):
        return ('low',)
    return ('native', 'low')


def completed_points(rows, recon=None):
    expected_low = set(recon or DEFAULT_RECON)
    methods = {}
    for row in rows:
        if row['status'] == 'ok':
            methods.setdefault(point_key(row), set()).add(row['reconstruction'])
    return {key for key, found in methods.items() if found == (
        {'native'} if key[2] == 'native' else expected_low)}


def verify_resume(previous, current):
    for key in ('config', 'rkvc_sha256', 'library_hashes', 'model_hashes', 'ffmpeg_version', 'ffprobe_version', 'cpu_affinity'):
        if previous.get(key) != current.get(key):
            raise ValueError(f'cannot resume: {key} changed; use a new output directory')
    for key in ('hostname', 'machine', 'device_tree_compatible'):
        if previous['system'].get(key) != current['system'].get(key):
            raise ValueError(f'cannot resume: board {key} changed')


def validate(config):
    for key in ('frames', 'iterations', 'width', 'height', 'scale'):
        value = config[key]
        if type(value) is not int or value <= 0:
            raise ValueError(f'{key} must be a positive integer')
    if type(config['warmup']) is not int or config['warmup'] < 0:
        raise ValueError('warmup must be a nonnegative integer')
    if 'gop' in config:
        if type(config['gop']) is not int or not 1 <= config['gop'] <= 10000:
            raise ValueError('gop must be an integer in 1..10000')
        if type(config['fps']) is not int or not 1 <= config['fps'] <= 240:
            raise ValueError('explicit encoder fps must be an integer in 1..240')
    for key in ('timeout_seconds', 'fps'):
        if not math.isfinite(config[key]) or config[key] <= 0:
            raise ValueError(f'{key} must be finite and positive')
    for key in ('width', 'height'):
        if config[key] % (2 * config['scale']):
            raise ValueError('NV12 dimensions must be even at both resolutions')
    if not config['sequences'] or not config['codecs']:
        raise ValueError('sequences and codecs cannot be empty')
    names = [s['name'] for s in config['sequences']]
    if len(names) != len(set(names)) or any(not re.fullmatch(r'[A-Za-z0-9_-]+', n) for n in names):
        raise ValueError('sequence names must be unique safe filenames')
    for codec, qps in config['codecs'].items():
        limit = 51 if codec in ('h264', 'hevc') else 63
        if codec not in ('h264', 'hevc', 'av1') + MLVC_CODECS or len(set(qps)) < 4:
            raise ValueError('each supported codec needs at least four distinct QPs')
        if any(type(q) is not int or not 0 <= q <= limit for q in qps):
            raise ValueError(f'invalid QP for {codec}')
    if config.get('classical_decoder', 'rkvc') not in ('rkvc', 'ffmpeg-rkmpp'):
        raise ValueError('classical_decoder must be rkvc or ffmpeg-rkmpp')
    if type(config.get('mlvc_alignment', 16)) is not int or config.get('mlvc_alignment', 16) <= 0:
        raise ValueError('mlvc_alignment must be a positive integer')
    if not config.get('sr_model') or not config.get('mlvc_model') or not config.get('mlvc_dec_model'):
        raise ValueError('explicit sr_model, mlvc_model and mlvc_dec_model IDs are required')
    for codec in MLVC_CODECS:
        if codec in config['codecs']:
            key = 'mlvc_s_models' if codec == 'mlvc-s' else 'mlvc_models'
            models = config.get(key, {})
            if any(not isinstance(models.get(str(q)), str) or not models[str(q)]
                   for q in config['codecs'][codec]):
                raise ValueError(f'{key} must explicitly map every tested QP to its matching model ID')
            dkey = 'mlvc_s_dec_models' if codec == 'mlvc-s' else 'mlvc_dec_models'
            decs = config.get(dkey, {})
            if any(not isinstance(decs.get(str(q)), str) or not decs[str(q)]
                   for q in config['codecs'][codec]):
                raise ValueError(f'{dkey} must explicitly map every tested QP to its matching model ID')
    recon = config.get('reconstructions', list(DEFAULT_RECON))
    if type(recon) is not list or not recon or len(set(recon)) != len(recon) \
            or any(m not in DEFAULT_RECON for m in recon):
        raise ValueError('reconstructions must be distinct entries from low_native/bicubic/lanczos/sr')
    if 'low_native' not in recon:
        raise ValueError('reconstructions must include low_native')
    if 'sr' in recon and config.get('scale', 1) != 3:
        raise ValueError('the fixed-3x SR node requires scale 3 for sr reconstructions')
    if type(config.get('native_reference', True)) is not bool:
        raise ValueError('native_reference must be a boolean')


class Runner:
    def __init__(self, config, output):
        self.c = config
        self.output = output
        self.commands = []

    def run(self, args):
        args = list(map(str, args))
        start = time.perf_counter()
        try:
            result = subprocess.run(args, capture_output=True, text=True, errors='replace',
                                    timeout=self.c['timeout_seconds'], check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            self.commands.append({'argv': args, 'seconds': time.perf_counter() - start,
                                  'returncode': None, 'error': str(exc)})
            raise
        elapsed = time.perf_counter() - start
        self.commands.append({'argv': args, 'seconds': elapsed,
                              'returncode': result.returncode, 'stderr': result.stderr[-8000:]})
        if result.returncode:
            raise RuntimeError(result.stderr[-2000:] or result.stdout[-2000:])
        return elapsed, result.stdout, result.stderr

    def raw(self, path, w, h):
        return ['-f', 'rawvideo', '-pixel_format', 'nv12', '-video_size', f'{w}x{h}',
                '-framerate', str(self.c['fps']), '-i', path]

    def ff(self):
        return [self.c['ffmpeg'], '-hide_banner', '-nostdin', '-y']

    def media(self, op, src, dst, codec=None, w=None, h=None, qp=None, model=None):
        # 新 C++ CLI（cpp-rewrite）：长选项，--model-dir 选择已注册的
        # 模型目录。--low-delay 已移除：MLVC 只有 P 帧，MPP 默认
        # 构造上就没有 B 帧（见 protocol）。
        args = [self.c['rkvc'], op, '--input', src, '--output', dst]
        if op == 'encode' and 'gop' in self.c:
            args += ['--gop', self.c['gop'], '--fps', self.c['fps']]
        if codec:
            args += ['--codec', 'mlvc' if codec == 'mlvc-s' else codec]
        if w:
            args += ['--width', w, '--height', h]
        if qp is not None:
            args += ['--qp', qp]
        if model:
            args += ['--model-id', model]
        if self.c.get('model_dir'):
            args += ['--model-dir', self.c['model_dir']]
        if self.c.get('backend_dir'):
            args += ['--backend-dir', self.c['backend_dir']]
        return args

    def check_raw(self, path, w, h):
        expected = w * h * 3 // 2 * self.c['frames']
        if path.stat().st_size != expected:
            raise ValueError(f'{path}: expected {expected} bytes (exact frame count/geometry), '
                             f'got {path.stat().st_size}')

    def audit_gop(self, stream, codec):
        """Verify actual keyframe positions; configuration alone is not evidence."""
        if 'gop' not in self.c:
            return None
        keys = []
        if codec in MLVC_CODECS:
            blob = stream.read_bytes()
            if blob[:6] != b'MLVC1\x02' or len(blob) < 64:
                raise ValueError('GOP audit requires MLVC container format 2')
            fps, den = struct.unpack_from('<II', blob, 16)
            gop, _, ltr = struct.unpack_from('<III', blob, 32)
            if (fps, den, gop, ltr) != (self.c['fps'], 1, self.c['gop'], 0):
                raise ValueError('MLVC stream timing/GOP/LTR mismatch')
            offset, count = 64, 0
            while offset < len(blob):
                if offset + 16 > len(blob):
                    raise ValueError('truncated MLVC record')
                size, qp, flags, _ = struct.unpack_from('<IiII', blob, offset)
                if not size or qp < 0 or offset + 16 + size > len(blob):
                    raise ValueError('invalid/dropped MLVC frame in RD measurement')
                if flags & 1:
                    keys.append(count)
                offset += 16 + size
                count += 1
            details = {'fps': fps, 'ltr_period': ltr}
        elif codec == 'av1':
            # rkmpp 不把 AV1 key_frame 标志传播到解码出的 AVFrame。
            # 改为直接读 AV1 frame_type 语法，而不是相信标志。
            _, _, stderr = self.run(self.ff() + ['-f', 'obu', '-i', stream,
                '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'])
            kinds = re.findall(r'\bframe_type\s+[01]+\s*=\s*(\d+)', stderr)
            count = len(kinds)
            keys = [i for i, kind in enumerate(kinds) if kind == '0']
            details = {'parser': 'ffmpeg/trace_headers', 'frame_types': sorted(set(kinds))}
        else:
            _, stdout, _ = self.run([self.c.get('ffprobe', 'ffprobe'), '-v', 'error',
                '-f', codec, '-i', stream, '-show_frames', '-show_entries',
                'frame=key_frame,pict_type', '-of', 'json'])
            frames = json.loads(stdout)['frames']
            keys = [i for i, frame in enumerate(frames) if frame.get('key_frame') == 1]
            count = len(frames)
            details = {'parser': 'ffprobe',
                       'picture_types': sorted({frame.get('pict_type', '?') for frame in frames})}
        expected = list(range(0, self.c['frames'], self.c['gop']))
        if count != self.c['frames'] or keys != expected:
            raise ValueError(f'GOP audit failed: {count} frames, keyframes {keys}; expected {expected}')
        return dict(details, keyframes=keys, frames=count, verified=True)

    def quality(self, ref, rec, work, dimensions=None):
        w, h = dimensions or (self.c['width'], self.c['height'])
        self.check_raw(rec, w, h)
        values = {}
        for metric in ('psnr', 'ssim'):
            log = work / f'{metric}.txt'
            # cwd 无关的 filter 文件名；转义 Windows 盘符与分隔符。
            escaped = log.resolve().as_posix().replace(':', r'\:').replace("'", r"'\''")
            self.run(self.ff() + self.raw(rec, w, h) + self.raw(ref, w, h) +
                     ['-lavfi', f"[0:v][1:v]{metric}=stats_file='{escaped}'",
                      '-f', 'null', '-'])
            lines = log.read_text().splitlines()
            if len(lines) != self.c['frames']:
                raise ValueError(f'{metric}: incomplete frame pairing')
            key = 'mse_y' if metric == 'psnr' else 'Y'
            matches = [re.search(rf'\b{key}:([^\s]+)', line) for line in lines]
            if not all(matches):
                raise ValueError(f'{metric}: missing per-frame {key} field')
            samples = [float(match[1]) for match in matches]
            if not all(math.isfinite(x) for x in samples):
                raise ValueError('nonfinite pixel metric')
            mean = statistics.fmean(samples)
            # Null 表示无损（+infinity）；保持 JSON 标准合规。
            values['psnr_y' if metric == 'psnr' else 'ssim_y'] = (
                (10 * math.log10(255 ** 2 / mean) if mean else None)
                if metric == 'psnr' else mean)
        return values

    def prepare(self, sequence, work):
        c = self.c
        src = Path(sequence['path'])
        frame_bytes = c['width'] * c['height'] * 3 // 2
        if src.stat().st_size % frame_bytes or src.stat().st_size < c['frames'] * frame_bytes:
            raise ValueError('source must contain enough complete 8-bit I420 frames')
        if sequence.get('sha256') and digest(src) != sequence['sha256']:
            raise ValueError('source SHA256 mismatch')
        ref, low = work / 'reference.nv12', work / 'low.nv12'
        self.run(self.ff() + ['-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size',
                 f"{c['width']}x{c['height']}", '-framerate', c['fps'], '-i', src,
                 '-frames:v', c['frames'], '-pix_fmt', 'nv12', '-f', 'rawvideo', ref])
        self.check_raw(ref, c['width'], c['height'])
        w, h = c['width'] // c['scale'], c['height'] // c['scale']
        seconds, _, _ = self.run(self.ff() + self.raw(ref, c['width'], c['height']) +
            ['-vf', f'scale={w}:{h}:flags=lanczos', '-pix_fmt', 'nv12', '-f', 'rawvideo', low])
        self.check_raw(low, w, h)
        return ref, low, seconds

    def point(self, codec, qp, branch, ref, low, down_seconds, work, sr_available):
        c = self.c
        native = branch == 'native'
        w, h = c['width'], c['height']
        if not native:
            w, h = w // c['scale'], h // c['scale']
        stream, decoded = work / ('coded.' + codec), work / 'decoded.nv12'
        dec_model = None
        if codec in MLVC_CODECS:
            key = 'mlvc_s_models' if codec == 'mlvc-s' else 'mlvc_models'
            dkey = 'mlvc_s_dec_models' if codec == 'mlvc-s' else 'mlvc_dec_models'
            model = c.get(key, {}).get(str(qp), c['mlvc_model'])
            dec_model = c.get(dkey, {}).get(str(qp), c['mlvc_dec_model'])
        else:
            model = None
        source = ref if native else low
        coded_w, coded_h = w, h
        padding_seconds = 0
        if codec in MLVC_CODECS:
            alignment = c.get('mlvc_alignment', 16)
            coded_w = (w + alignment - 1) // alignment * alignment
            coded_h = (h + alignment - 1) // alignment * alignment
            if (coded_w, coded_h) != (w, h):
                source = work / 'padded.nv12'
                padding_seconds, _, _ = self.run(self.ff() + self.raw(low, w, h) +
                    ['-vf', f'pad={coded_w}:{coded_h}:0:0,fillborders=right={coded_w-w}:bottom={coded_h-h}:mode=smear',
                     '-pix_fmt', 'nv12', '-f', 'rawvideo', source])
        raw_decoded = work / 'padded-decoded.nv12' if (coded_w, coded_h) != (w, h) else decoded
        enc = self.media('encode', source, stream, codec, coded_w, coded_h, qp, model)
        # 新 CLI 的 decode 把 --width/--height 当输出几何：编码后
        # （可能带填充）的尺寸，之后再裁剪回 w,h。
        dec = self.media('decode', stream, raw_decoded, codec, coded_w, coded_h,
                         model=dec_model)
        decoder = 'rkvc'
        if codec not in MLVC_CODECS and c.get('classical_decoder') == 'ffmpeg-rkmpp':
            decoder = f'ffmpeg/{codec}_rkmpp'
            dec = self.ff() + ['-hwaccel', 'rkmpp', '-hwaccel_output_format', 'drm_prime',
                '-c:v', f'{codec}_rkmpp', '-f',
                'obu' if codec == 'av1' else codec, '-i', stream,
                '-vf', 'hwdownload,format=nv12', '-fps_mode', 'passthrough',
                '-f', 'rawvideo', raw_decoded]
        samples = []
        sizes = []
        for i in range(c['warmup'] + c['iterations']):
            te, _, _ = self.run(enc)
            if not stream.is_file() or not stream.stat().st_size:
                raise ValueError('encoder produced no bitstream')
            td, _, _ = self.run(dec)
            self.check_raw(raw_decoded, coded_w, coded_h)
            if raw_decoded != decoded:
                tc, _, _ = self.run(self.ff() + self.raw(raw_decoded, coded_w, coded_h) +
                    ['-vf', f'crop={w}:{h}:0:0', '-pix_fmt', 'nv12', '-f', 'rawvideo', decoded])
                td += tc
            self.check_raw(decoded, w, h)
            if i >= c['warmup']:
                samples.append({'encode_seconds': te, 'decode_seconds': td})
                sizes.append(stream.stat().st_size)
        # 质量指标属于最终测得的码流；保留其字节/哈希。
        common = {'codec': codec, 'qp': qp, 'branch': branch, 'status': 'ok',
                  'bytes': sizes[-1], 'sample_bytes': sizes, 'stream_sha256': digest(stream),
                  'kbps': sizes[-1] * 8 * c['fps'] / c['frames'] / 1000,
                  'bpp': sizes[-1] * 8 / (c['frames'] * c['width'] * c['height']),
                  'samples': samples, 'downsample_seconds': 0 if native else down_seconds}
        common.update(coded_width=coded_w, coded_height=coded_h, padding_seconds=padding_seconds,
                      model_id=model, decoder=decoder)
        common['gop_audit'] = self.audit_gop(stream, codec)
        rows = []
        methods = ['native'] if native else list(c.get('reconstructions', DEFAULT_RECON))
        for method in methods:
            row = dict(common, reconstruction=method)
            try:
                up = []
                rec = decoded
                if method not in ('native', 'low_native'):
                    rec = work / f'{method}.nv12'
                    if method == 'sr':
                        if not sr_available:
                            raise ValueError('requested SR model is absent; fallback forbidden')
                        cmd = self.media('upscale', decoded, rec, w=w, h=h, model=c['sr_model'])
                    else:
                        kernel = ':param0=0:param1=0.75' if method == 'bicubic' else ''
                        cmd = self.ff() + self.raw(decoded, w, h) + ['-vf',
                            f"scale={c['width']}:{c['height']}:flags={method}{kernel}",
                            '-pix_fmt', 'nv12', '-f', 'rawvideo', rec]
                    for i in range(c['warmup'] + c['iterations']):
                        t, _, _ = self.run(cmd)
                        self.check_raw(rec, c['width'], c['height'])
                        if i >= c['warmup']:
                            up.append(t)
                row.update(self.quality(low if method == 'low_native' else ref, rec, work,
                                       (w, h) if method == 'low_native' else None))
                row['metric_width'] = w if method == 'low_native' else c['width']
                row['metric_height'] = h if method == 'low_native' else c['height']
                row['bpp'] = row['bytes'] * 8 / (c['frames'] * row['metric_width'] * row['metric_height'])
                row['upscale_samples_seconds'] = up
                row['encode_fps'] = c['frames'] / statistics.fmean(s['encode_seconds'] for s in samples)
                row['decode_fps'] = c['frames'] / statistics.fmean(s['decode_seconds'] for s in samples)
                # 各进程阶段分别计时的总和，并非流式流水线的测量。
                row['stage_sum_seconds'] = (statistics.fmean(s['encode_seconds'] + s['decode_seconds']
                    for s in samples) + (statistics.fmean(up) if up else 0) + row['downsample_seconds'] + padding_seconds)
                row['stage_sum_fps'] = c['frames'] / row['stage_sum_seconds']
                row['realtime_speed'] = row['stage_sum_fps'] / c['fps']
            except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
                row.update(status='failed', error=str(exc))
            rows.append(row)
        return rows


def save(report, output):
    temporary = output / 'rd.json.tmp'
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False,
                                             allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(output / 'rd.json')
    fields = ['sequence', 'codec', 'qp', 'branch', 'reconstruction', 'status', 'kbps', 'bpp',
              'psnr_y', 'ssim_y', 'encode_fps', 'decode_fps', 'stage_sum_fps', 'realtime_speed', 'error']
    with (output / 'rd.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(report['points'])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True, help='new run directory; never overwrites a run')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--preflight', action='store_true', help='two frames from the first sequence; all codec/QP paths')
    p.add_argument('--resume', action='store_true', help='resume an existing run after checking provenance')
    args = p.parse_args(argv)
    c = json.loads(args.config.read_text(encoding='utf-8'))
    validate(c)
    if args.preflight:
        c.update(frames=min(2, c['frames']), warmup=0, iterations=1,
                 sequences=c['sequences'][:1], dataset=c['dataset'] + ' / PREFLIGHT ONLY')
    for key in ('rkvc', 'ffmpeg', 'model_dir'):
        if c.get(key) and ('/' in c[key] or '\\' in c[key]):
            c[key] = str((args.config.resolve().parent / c[key]).resolve())
    for seq in c['sequences']:
        seq['path'] = str((args.config.resolve().parent / seq['path']).resolve())
    if args.dry_run:
        print(json.dumps(c, indent=2))
        print('native + low-resolution: bicubic / lanczos / explicit SR; all four codecs')
        return 0
    if c.get('cpu_affinity'):
        if not hasattr(os, 'sched_setaffinity'):
            raise ValueError('cpu_affinity requires Linux')
        os.sched_setaffinity(0, set(c['cpu_affinity']))
    previous = json.loads((args.output / 'rd.json').read_text(encoding='utf-8')) if args.resume else None
    args.output.mkdir(parents=True, exist_ok=args.resume)
    write_allowed = previous is None
    runner = Runner(c, args.output)
    report = {'schema_version': 1, 'system': system_metadata(), 'config': c,
              'runner_sha256': digest(Path(__file__)),
              'measurement': 'board process wall time; separately timed stage sum includes file IO/init',
              'protocol': 'rkvc backend defaults; no matched GOP/FPS claim',
              'points': [], 'commands': runner.commands, 'sources': []}
    if hasattr(os, 'sched_getaffinity'):
        report['cpu_affinity'] = sorted(os.sched_getaffinity(0))
    try:
        report['rkvc_sha256'] = digest(shutil.which(c['rkvc']) or c['rkvc'])
        package = Path(shutil.which(c['rkvc']) or c['rkvc']).resolve().parent.parent
        report['library_hashes'] = {str(f.relative_to(package)): digest(f)
                                   for f in (package / 'lib').rglob('*.so*')
                                   if f.is_file() and not f.is_symlink()}
        report['version'] = runner.run([c['rkvc'], 'version', '--json'])[1]
        backends = [c['rkvc'], 'inspect', 'backends', '--json']
        models_cmd = [c['rkvc'], 'inspect', 'models', '--json']
        # inspect models 与 encode/decode 一样解析 --model-dir。
        model_dir = Path(c['model_dir']) if c.get('model_dir') else Path(c['rkvc']).resolve().parent.parent / 'share/rkvc/models'
        report['model_hashes'] = {str(f): digest(f)
                                  for f in model_dir.rglob('*.rknn')}
        if c.get('model_dir'):
            models_cmd += ['--model-dir', c['model_dir']]
        if c.get('backend_dir'):
            backends += ['--backend-dir', c['backend_dir']]
        report['backends'] = runner.run(backends)[1]
        report['models'] = runner.run(models_cmd)[1]
        models = model_listing(report['models'])
        sr_available = any(m['id'] == c['sr_model'] and m['role'] == 'upscale' for m in models)
        report['ffmpeg_version'] = runner.run([c['ffmpeg'], '-version'])[1]
        if 'gop' in c:
            report['ffprobe_version'] = runner.run([c.get('ffprobe', 'ffprobe'), '-version'])[1]
            report['protocol'] = ('new C++ CLI; explicit fps/GOP; no B-frames by construction '
                                  '(MLVC P-only, MPP defaults); MLVC LTR off; verified keyframe positions')
        if previous is not None:
            verify_resume(previous, report)
            backup = args.output / f'rd.before-resume-{time.time_ns()}.json'
            shutil.copyfile(args.output / 'rd.json', backup)
            report['sessions'] = previous.get('sessions', [{
                'system': previous['system'], 'system_end': previous.get('system_end'),
                'runner_sha256': previous.get('runner_sha256')}]) + [{
                'system': report['system'], 'runner_sha256': report['runner_sha256']}]
            report['system'] = previous['system']
            report['points'] = [r for r in previous['points'] if r['status'] == 'ok']
            report['previous_failures'] = previous.get('previous_failures', []) + [
                r for r in previous['points'] if r['status'] != 'ok']
            report['sources'] = previous['sources']
            runner.commands[:0] = previous['commands']
            write_allowed = True
        done = completed_points(report['points'], c.get('reconstructions'))
        for sequence in c['sequences']:
            planned = {(sequence['name'], codec, branch, qp)
                       for codec, qps in c['codecs'].items()
                       for branch in planned_branches(c, codec) for qp in qps}
            if planned <= done:
                continue
            work = args.output / sequence['name']
            work.mkdir(exist_ok=args.resume)
            try:
                ref, low, down = runner.prepare(sequence, work)
                ref_hash = digest(ref)
                old_source = next((s for s in report['sources'] if s['sequence'] == sequence['name']), None)
                if old_source and old_source['reference_sha256'] != ref_hash:
                    raise ValueError('prepared reference changed since the previous session')
                report['sources'] = [s for s in report['sources'] if s['sequence'] != sequence['name']]
                report['sources'].append({'sequence': sequence['name'], 'reference_sha256': ref_hash,
                                          'source_bytes': Path(sequence['path']).stat().st_size})
                for codec, qps in c['codecs'].items():
                    for branch in planned_branches(c, codec):
                        for qp in qps:
                            key = (sequence['name'], codec, branch, qp)
                            if key in done:
                                continue
                            progress(f"{sequence['name']} {codec} {branch} QP={qp}")
                            point_work = work / f'{codec}-{branch}-{qp}'
                            point_work.mkdir(exist_ok=args.resume)
                            try:
                                rows = runner.point(codec, qp, branch, ref, low, down, point_work, sr_available)
                            except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
                                rows = [{'codec': codec, 'qp': qp, 'branch': branch,
                                         'status': 'failed', 'error': str(exc)}]
                            report['points'] = [r for r in report['points'] if point_key(r) != key]
                            report['points'].extend(dict(row, sequence=sequence['name']) for row in rows)
                            save(report, args.output)
                            # 只删除本次全新运行的 point 目录中创建的文件。
                            if not c.get('keep_media', False):
                                for f in point_work.iterdir():
                                    if f.is_file():
                                        f.unlink()
            except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
                report['points'].append({'sequence': sequence['name'], 'status': 'failed', 'error': str(exc)})
            if not c.get('keep_media', False):
                for filename in ('reference.nv12', 'low.nv12'):
                    (work / filename).unlink(missing_ok=True)
            save(report, args.output)
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        report['fatal_error'] = str(exc)
    finally:
        report['system_end'] = system_metadata()
        if report.get('sessions'):
            report['sessions'][-1]['system_end'] = report['system_end']
        if write_allowed:
            save(report, args.output)
        else:
            (args.output / 'resume-rejected.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return int(bool(report.get('fatal_error')) or any(r['status'] != 'ok' for r in report['points']))


if __name__ == '__main__':
    sys.exit(main())
