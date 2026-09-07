#!/usr/bin/env python3
"""Download official UVG-7 archives and prepare a checksummed I420 frame prefix.

Requires py7zr on the preparation host, never on the board. Downloaded originals
and extracted files stay in the cache for reproducibility and reuse.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import tempfile
import urllib.request

try:
    from .rd import digest
except ImportError:
    from rd import digest


def download(url, output, jobs):
    """Use verified byte ranges; retain partial chunks for a restarted download."""
    request = urllib.request.Request(url, method='HEAD')
    with urllib.request.urlopen(request, timeout=60) as response:
        size = int(response.headers['Content-Length'])
    if output.exists() and output.stat().st_size == size:
        return
    chunk_size = 16 * 1024 * 1024
    parts = list(range((size + chunk_size - 1) // chunk_size))

    def fetch(index):
        start, end = index * chunk_size, min(size, (index + 1) * chunk_size) - 1
        part = output.with_suffix(f'.part{index:04d}')
        if part.exists() and part.stat().st_size == end - start + 1:
            return part
        for attempt in range(3):
            try:
                req = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}'})
                with urllib.request.urlopen(req, timeout=120) as response:
                    if response.status != 206 or response.headers.get('Content-Range') != f'bytes {start}-{end}/{size}':
                        raise ValueError('server did not honor requested byte range')
                    with part.open('wb') as stream:
                        shutil.copyfileobj(response, stream)
                if part.stat().st_size != end - start + 1:
                    raise ValueError('truncated download')
                return part
            except (OSError, ValueError):
                if attempt == 2:
                    raise

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        paths = list(pool.map(fetch, parts))
    temporary = output.with_suffix('.assembling')
    with temporary.open('wb') as stream:
        for part in paths:
            with part.open('rb') as source:
                shutil.copyfileobj(source, stream)
    temporary.replace(output)
    for part in paths:
        part.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('rd.uvg.json'))
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='prepared manifest directory')
    parser.add_argument('--sequence', action='append', help='optional subset; never labelled full UVG-7')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be between 1 and 32')
    import py7zr
    c = json.loads(args.config.read_text(encoding='utf-8'))
    if args.sequence:
        unknown = set(args.sequence) - {s['name'] for s in c['sequences']}
        if unknown:
            parser.error(f'unknown sequences: {sorted(unknown)}')
        c['sequences'] = [s for s in c['sequences'] if s['name'] in args.sequence]
        c['dataset'] += ' / SUBSET: ' + ', '.join(args.sequence)
    args.cache.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    prefix_bytes = c['width'] * c['height'] * 3 // 2 * c['frames']
    prepared = []
    for seq in c['sequences']:
        archive = args.cache / (seq['name'] + '.7z')
        print(f"Download / verify {seq['name']}", flush=True)
        download(seq['download_url'], archive, args.jobs)
        with tempfile.TemporaryDirectory(dir=args.cache) as temporary:
            with py7zr.SevenZipFile(archive) as z:
                names = z.getnames()
                targets = [n for n in names if n.lower().endswith('.yuv')]
                if len(targets) != 1 or any(ch in targets[0] for ch in '/\\:') or targets[0] in ('.', '..'):
                    raise ValueError(f'unexpected UVG archive layout: {names}')
                z.extract(path=temporary, targets=targets)
            source = Path(temporary) / targets[0]
            if source.stat().st_size < prefix_bytes:
                raise ValueError('source is shorter than requested prefix')
            target = args.output / f"{seq['name']}.yuv"
            with source.open('rb') as src, target.open('wb') as dst:
                remaining = prefix_bytes
                while remaining:
                    block = src.read(min(1024 * 1024, remaining))
                    if not block:
                        raise ValueError('truncated source')
                    dst.write(block)
                    remaining -= len(block)
            seq.update(path=target.name, sha256=digest(target), archive_sha256=digest(archive),
                       archive_member=targets[0], source_frames=source.stat().st_size // (c['width'] * c['height'] * 3 // 2))
        print(f"Prepared {seq['name']}: {seq['sha256']}", flush=True)
        prepared.append(seq)
        # A durable progress manifest is explicitly a subset until all finish.
        progress = dict(c, sequences=list(prepared), dataset=c['dataset'] + ' / preparation in progress')
        (args.output / 'rd.progress.json').write_text(json.dumps(progress, indent=2) + '\n', encoding='utf-8')
    (args.output / 'rd.prepared.json').write_text(json.dumps(c, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
