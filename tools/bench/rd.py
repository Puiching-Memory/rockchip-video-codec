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
    # Some vendor libraries print startup banners to stdout before CLI JSON.
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


def validate(config):
    for key in ('frames', 'iterations', 'width', 'height', 'scale'):
        value = config[key]
        if type(value) is not int or value <= 0:
            raise ValueError(f'{key} must be a positive integer')
    if type(config['warmup']) is not int or config['warmup'] < 0:
        raise ValueError('warmup must be a nonnegative integer')
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
        if codec not in ('h264', 'hevc', 'av1', 'mlvc') or len(set(qps)) < 4:
            raise ValueError('each supported codec needs at least four distinct QPs')
        if any(type(q) is not int or not 0 <= q <= limit for q in qps):
            raise ValueError(f'invalid QP for {codec}')
    if config.get('classical_decoder', 'rkvc') not in ('rkvc', 'ffmpeg-rkmpp'):
        raise ValueError('classical_decoder must be rkvc or ffmpeg-rkmpp')
    if type(config.get('mlvc_alignment', 16)) is not int or config.get('mlvc_alignment', 16) <= 0:
        raise ValueError('mlvc_alignment must be a positive integer')
    if not config.get('sr_model') or not config.get('mlvc_model'):
        raise ValueError('explicit sr_model and mlvc_model IDs are required')
    if 'mlvc' in config['codecs']:
        models = config.get('mlvc_models', {})
        if any(not isinstance(models.get(str(q)), str) or not models[str(q)]
               for q in config['codecs']['mlvc']):
            raise ValueError('mlvc_models must explicitly map every tested QP to its matching model ID')


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
        args = [self.c['rkvc'], op, '-i', src, '-o', dst]
        if codec:
            args += ['--codec', codec]
        if w:
            args += ['--width', w, '--height', h]
        if qp is not None:
            args += ['--qp', qp]
        if model:
            args += ['--model', model]
        if self.c.get('model_dir'):
            args += ['--model-dir', self.c['model_dir']]
        return args

    def check_raw(self, path, w, h):
        expected = w * h * 3 // 2 * self.c['frames']
        if path.stat().st_size != expected:
            raise ValueError(f'{path}: expected {expected} bytes (exact frame count/geometry), '
                             f'got {path.stat().st_size}')

    def quality(self, ref, rec, work, dimensions=None):
        w, h = dimensions or (self.c['width'], self.c['height'])
        self.check_raw(rec, w, h)
        values = {}
        for metric in ('psnr', 'ssim'):
            log = work / f'{metric}.txt'
            # cwd-independent filter filename; escape Windows drive and separators.
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
            # Null means lossless (+infinity); keep JSON standards compliant.
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
        model = c.get('mlvc_models', {}).get(str(qp), c['mlvc_model']) if codec == 'mlvc' else None
        source = ref if native else low
        coded_w, coded_h = w, h
        padding_seconds = 0
        if codec == 'mlvc':
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
        dec = self.media('decode', stream, raw_decoded, codec, model=model)
        decoder = 'rkvc'
        if codec != 'mlvc' and c.get('classical_decoder') == 'ffmpeg-rkmpp':
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
        # Quality belongs to the final measured stream; retain its bytes/hash.
        common = {'codec': codec, 'qp': qp, 'branch': branch, 'status': 'ok',
                  'bytes': sizes[-1], 'sample_bytes': sizes, 'stream_sha256': digest(stream),
                  'kbps': sizes[-1] * 8 * c['fps'] / c['frames'] / 1000,
                  'bpp': sizes[-1] * 8 / (c['frames'] * c['width'] * c['height']),
                  'samples': samples, 'downsample_seconds': 0 if native else down_seconds}
        common.update(coded_width=coded_w, coded_height=coded_h, padding_seconds=padding_seconds,
                      model_id=model, decoder=decoder)
        rows = []
        methods = ['native'] if native else ['low_native', 'bicubic', 'lanczos', 'sr']
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
                # Sum of separately timed process stages, not a streaming pipeline measurement.
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
    args.output.mkdir(parents=True, exist_ok=False)
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
        report['backends'] = runner.run([c['rkvc'], 'inspect', 'backends', '--json'])[1]
        # Model listing follows context override via the environment, as inspect has no path flag.
        model_dir = Path(c['model_dir']) if c.get('model_dir') else Path(c['rkvc']).resolve().parent.parent / 'share/rkvc/models'
        report['model_hashes'] = {str(f): digest(f) for f in model_dir.glob('*.rkmodel')}
        report['models'] = runner.run([c['rkvc'], 'inspect', 'models', '--json'])[1]
        models = model_listing(report['models'])
        # A custom model directory cannot be verified through inspect; require default registry for now.
        if c.get('model_dir'):
            raise ValueError('RD runner currently requires the package default model registry')
        sr_available = any(m['id'] == c['sr_model'] and m['role'] == 'upscale' for m in models)
        report['ffmpeg_version'] = runner.run([c['ffmpeg'], '-version'])[1]
        for sequence in c['sequences']:
            work = args.output / sequence['name']
            work.mkdir()
            try:
                ref, low, down = runner.prepare(sequence, work)
                report['sources'].append({'sequence': sequence['name'], 'reference_sha256': digest(ref),
                                          'source_bytes': Path(sequence['path']).stat().st_size})
                for codec, qps in c['codecs'].items():
                    for branch in (('low',) if codec == 'mlvc' else ('native', 'low')):
                        for qp in qps:
                            print(f"{sequence['name']} {codec} {branch} QP={qp}", flush=True)
                            point_work = work / f'{codec}-{branch}-{qp}'
                            point_work.mkdir()
                            try:
                                rows = runner.point(codec, qp, branch, ref, low, down, point_work, sr_available)
                            except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
                                rows = [{'codec': codec, 'qp': qp, 'branch': branch,
                                         'status': 'failed', 'error': str(exc)}]
                            report['points'].extend(dict(row, sequence=sequence['name']) for row in rows)
                            save(report, args.output)
                            # Only delete files created in this fresh run's point directory.
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
        save(report, args.output)
    return int(bool(report.get('fatal_error')) or any(r['status'] != 'ok' for r in report['points']))


if __name__ == '__main__':
    sys.exit(main())
