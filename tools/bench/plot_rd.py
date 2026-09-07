#!/usr/bin/env python3
"""Publish measured RD data as a readable static report (matplotlib required)."""
import argparse
from collections import defaultdict
import html
import json
import math
import statistics
from pathlib import Path

COLORS = {'h264': '#2878b5', 'hevc': '#d58a18', 'av1': '#259c83', 'mlvc': '#9254cf'}
LABELS = {'h264': 'H.264 · MPP', 'hevc': 'H.265 · MPP', 'av1': 'AV1 · SVT', 'mlvc': 'MLVC · NPU'}
STYLES = {'native': ('-', 'o'), 'low_native': ('-', 'o'), 'bicubic': ('--', 's'), 'lanczos': (':', '^'), 'sr': ('-', 'D')}


def groups(points):
    grouped = defaultdict(list)
    for row in points:
        if row['status'] == 'ok':
            grouped[(row['sequence'], row['codec'], row['reconstruction'])].append(row)
    return grouped


def aggregate(points, sequence_names):
    """Equal-length sequences only; never aggregate an incomplete subset."""
    buckets = defaultdict(dict)
    for row in points:
        if row['status'] == 'ok':
            buckets[(row['codec'], row['qp'], row['reconstruction'])][row['sequence']] = row
    result = []
    for (codec, qp, method), rows in buckets.items():
        if set(rows) != set(sequence_names):
            continue
        values = list(rows.values())
        row = dict(values[0], sequence='Suite aggregate')
        mse = statistics.fmean(0 if r['psnr_y'] is None else 10 ** (-r['psnr_y'] / 10) for r in values)
        row['psnr_y'] = -10 * math.log10(mse) if mse else None
        for key in ('kbps', 'bpp', 'ssim_y'):
            row[key] = statistics.fmean(r[key] for r in values)
        for key in ('stage_sum_fps', 'realtime_speed'):
            row[key] = statistics.harmonic_mean(r[key] for r in values)
        result.append(row)
    return result


def render(report, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import ScalarFormatter

    output.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 11,
                        'axes.spines.top': False, 'axes.spines.right': False,
                        'axes.labelcolor': '#526070', 'text.color': '#182535',
                        'axes.edgecolor': '#cdd6de', 'xtick.color': '#526070',
                        'ytick.color': '#526070', 'figure.facecolor': '#f3f6fa',
                        'axes.facecolor': '#ffffff', 'savefig.facecolor': '#f3f6fa'})
    sequences = [s['name'] for s in report['config']['sequences']]
    pooled = aggregate(report['points'], sequences) if len(sequences) > 1 else []
    data = groups(report['points'] + pooled)
    if pooled:
        sequences = ['Suite aggregate'] + sequences
    images = []
    for index, seq in enumerate(sequences):
        # Separate the codec overview from SR ablations to avoid a 16-line legend.
        panels = [('native', 'Full resolution anchors', list(COLORS)),
                  ('low_native', 'Low resolution codec comparison', list(COLORS)),
                  ('delivery', '1080p delivery comparison', list(COLORS))] + [
            ('low', LABELS[c] + ' · reconstruction', [c]) for c in COLORS]
        for branch, title, codecs in panels:
            fig, axes = plt.subplots(1, 2, figsize=(12.8, 5.2), layout='constrained')
            fig.suptitle(f'{seq}  /  {title}', fontsize=18, fontweight='bold')
            for ax, metric, label in zip(axes, ('psnr_y', 'ssim_y'), ('Y-PSNR (dB)', 'Y-SSIM')):
                count = 0
                for codec in codecs:
                    methods = [branch] if branch in ('native', 'low_native') else ['bicubic', 'lanczos', 'sr']
                    if branch == 'delivery':
                        methods = ['bicubic', 'lanczos', 'sr'] if codec == 'mlvc' else ['native']
                    for method in methods:
                        rows = sorted(data.get((seq, codec, method), []), key=lambda r: r['kbps'])
                        rows = [r for r in rows if r.get(metric) is not None and math.isfinite(r[metric])]
                        if not rows:
                            continue
                        style, marker = STYLES[method]
                        ax.plot([r['kbps'] for r in rows], [r[metric] for r in rows],
                                linestyle=style, marker=marker, color=COLORS[codec],
                                linewidth=2.3, markersize=6,
                                alpha=1 if method in ('native', 'sr') else .65,
                                label=(f'MLVC + {method}' if branch == 'delivery' and codec == 'mlvc' else
                                      LABELS[codec]) if method in ('native', 'low_native') or branch == 'delivery' else
                                      ('SR · ' + report['config']['sr_model'] if method == 'sr' else method))
                        count += 1
                ax.set(xscale='log', xlabel='Actual bitstream rate (kbps)', ylabel=label)
                ax.xaxis.set_major_formatter(ScalarFormatter())
                ax.grid(True, alpha=.22, linestyle='--', which='both')
                if count:
                    ax.legend(loc='best', frameon=False, fontsize=9)
                else:
                    ax.text(.5, .5, 'No valid measurements', ha='center', va='center', transform=ax.transAxes)
            name = f'{index:02d}-{branch}-' + ('codecs' if branch in ('native', 'low_native', 'delivery') else codecs[0])
            for extension in ('png', 'svg'):
                fig.savefig(output / f'{name}.{extension}', dpi=180)
            plt.close(fig)
            images.append((seq, branch, name + '.png'))

        fig, axes = plt.subplots(1, 2, figsize=(12.8, 5.5), layout='constrained')
        fig.suptitle(f'{seq}  /  Throughput vs. bitrate', fontsize=18, fontweight='bold')
        for ax, metric, label in zip(axes, ('stage_sum_fps', 'realtime_speed'),
                                     ('Stage-sum throughput (fps)', 'Realtime speed (×, higher is faster)')):
            for (s, codec, method), rows in data.items():
                if s != seq or method == 'low_native':
                    continue
                rows = sorted(rows, key=lambda r: r['kbps'])
                style, marker = STYLES[method]
                ax.plot([r['kbps'] for r in rows], [r[metric] for r in rows],
                        color=COLORS[codec], linestyle=style, marker=marker, markersize=4,
                        label=f'{codec} / {method}')
            ax.set(xscale='log', yscale='log', xlabel='Actual bitstream rate (kbps)', ylabel=label)
            ax.grid(True, alpha=.22, which='both')
        axes[1].axhline(1, color='#dc5262', linewidth=1, linestyle='--')
        handles, labels = axes[0].get_legend_handles_labels()
        if handles:
            fig.legend(handles, labels, loc='outside lower center', ncol=4, frameon=False, fontsize=8)
        name = f'{index:02d}-performance'
        for extension in ('png', 'svg'):
            fig.savefig(output / f'{name}.{extension}', dpi=180)
        plt.close(fig)
        images.append((seq, 'performance', name + '.png'))

    failed = [p for p in report['points'] if p['status'] != 'ok']
    esc = lambda x: html.escape(str(x))
    body = ''.join(f'<figure data-sequence="{esc(seq)}" data-panel="{branch}"><img loading="lazy" src="{name}" alt="{esc(seq)} {branch}"><figcaption><a href="{name[:-4]}.svg">SVG</a> · <a href="{name}">PNG</a></figcaption></figure>' for seq, branch, name in images)
    options = ''.join(f'<option>{esc(seq)}</option>' for seq in sequences)
    failures = ''.join('<tr>' + ''.join(f'<td>{esc(p.get(k, "—"))}</td>' for k in
                     ('sequence', 'codec', 'qp', 'branch', 'reconstruction', 'error')) + '</tr>' for p in failed)
    expected = len(report['config']['sequences']) * sum(len(qps) * (4 if c == 'mlvc' else 5)
                                                       for c, qps in report['config']['codecs'].items())
    valid = sum(p['status'] == 'ok' for p in report['points'])
    state = 'INCOMPLETE' if failed or report.get('fatal_error') or valid != expected else 'MEASURED'
    page = f'''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>RKVC / RD Lab</title>
<style>body{{margin:0;background:#f3f6fa;color:#182535;font:16px system-ui}}main{{max-width:1280px;margin:auto;padding:48px 24px}}
header{{border-top:5px solid #9254cf;padding-top:24px}}small{{letter-spacing:.15em;color:#66768a}}h1{{font-size:48px;margin:12px 0}}
p{{line-height:1.7;color:#526070}}figure{{margin:28px 0;background:white;border:1px solid #dde4ec;border-radius:16px;overflow:hidden}}
img{{width:100%;display:block}}details{{background:white;padding:20px;border-radius:12px;overflow:auto}}td,th{{padding:10px;text-align:left;border-bottom:1px solid #dde4ec}}pre{{white-space:pre-wrap}}
.cards{{display:flex;gap:16px;flex-wrap:wrap;margin:28px 0}}.card{{background:white;border:1px solid #dde4ec;border-radius:14px;padding:22px;flex:1;min-width:160px}}.card strong{{display:block;font-size:30px;margin-top:10px}}nav{{display:flex;gap:16px;flex-wrap:wrap;position:sticky;top:0;background:#f3f6faf5;padding:16px 0;z-index:2}}select{{padding:12px;border:1px solid #cdd6de;border-radius:10px;background:white;color:#182535;font:inherit}}a{{color:#7450b9}}figcaption{{padding:10px 20px;text-align:right}}[hidden]{{display:none!important}}@media(max-width:600px){{h1{{font-size:34px}}main{{padding:24px 12px}}}}</style>
<main><header><small>ROCKCHIP VIDEO CODEC / BENCHMARK LAB</small><h1>Rate. Quality. Reality.</h1>
<p>{state} · {esc(report['system'].get('hostname'))} · {esc(report['system'].get('timestamp_utc'))}<br>
{esc(report['config']['dataset'])} · {report['config']['frames']} frames / sequence ·
{report['config']['width']}×{report['config']['height']} · {report['config']['fps']} fps</p></header>
<div class="cards"><div class="card"><small>MEASURED POINTS</small><strong>{valid} / {expected}</strong></div><div class="card"><small>SEQUENCES</small><strong>{len(report['config']['sequences'])}</strong></div><div class="card"><small>UPSCALE</small><strong>{report['config']['scale']}×</strong></div><div class="card"><small>SAMPLES / WARMUP</small><strong>{report['config']['iterations']} / {report['config']['warmup']}</strong></div></div>
<p>Each sequence is plotted separately. Low-resolution methods share the same encoded bytes.
Y-PSNR uses pooled luma MSE; Y-SSIM is the mean frame score. Missing measurements are not interpolated.
Suite aggregate requires every configured sequence at each rate point and pools luma MSE across sequences.
Stage-sum time includes process startup, model initialization, file I/O and downsampling; quality scoring is excluded.
Backend GOP/FPS defaults differ; this is an implementation comparison.</p>
<details><summary>Provenance and configuration</summary><pre>{esc(json.dumps({k: v for k, v in report.items() if k not in ('points', 'commands')}, indent=2))}</pre></details>
<nav><label>Sequence <select id="sequence">{options}</select></label><label>View <select id="panel"><option value="all">All comparisons</option><option value="delivery">1080p delivery comparison</option><option value="low_native">Codec RD · low resolution</option><option value="native">1080p anchors</option><option value="low">Super-resolution ablation</option><option value="performance">Throughput</option></select></label></nav>
{body}<details open><summary>Unavailable measurements ({len(failed)})</summary><p>{esc(report.get('fatal_error', ''))}</p>
<table><tr><th>Sequence</th><th>Codec</th><th>QP</th><th>Branch</th><th>Reconstruction</th><th>Reason</th></tr>{failures}</table></details></main>
<script>function filter(){{const s=document.getElementById('sequence').value,p=document.getElementById('panel').value;document.querySelectorAll('figure').forEach(f=>f.hidden=f.dataset.sequence!==s||(p!=='all'&&f.dataset.panel!==p));}}document.querySelectorAll('select').forEach(s=>s.addEventListener('change',filter));filter();</script></html>'''
    (output / 'index.html').write_text(page, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    render(json.loads(args.report.read_text(encoding='utf-8')), args.output)
