#!/usr/bin/env python3
"""Publish measured RD data as a readable static report (matplotlib required)."""
import argparse
from collections import defaultdict
import html
import json
import math
import statistics
from string import Template
from pathlib import Path

COLORS = {'h264': '#2878b5', 'hevc': '#d58a18', 'av1': '#259c83', 'mlvc': '#9254cf', 'mlvc-s': '#c2457e'}
LABELS = {'h264': 'H.264 · MPP', 'hevc': 'H.265 · MPP', 'av1': 'AV1 · SVT', 'mlvc': 'MLVC · NPU', 'mlvc-s': 'MLVC-S · NPU'}
STYLES = {'native': ('-', 'o'), 'low_native': ('-', 'o'), 'bicubic': ('--', 's'), 'lanczos': (':', '^'), 'sr': ('-', 'D')}
METHOD_LABELS = {'native': '原生分辨率', 'low_native': '编码分辨率',
                 'bicubic': '双三次插值', 'lanczos': 'Lanczos 插值', 'sr': 'SR 超分辨率'}
PANEL_LABELS = {'native': '原生编码参照', 'low_native': '编码 RD',
                'delivery': '输出对比', 'low': '超分辨率消融', 'performance': '吞吐性能'}


def sequence_label(name):
    return '全测试集汇总' if name == 'Suite aggregate' else name


def panel_methods(branch, codec):
    if branch in ('native', 'low_native'):
        return [branch]
    if branch == 'delivery':
        return ['bicubic', 'lanczos', 'sr'] if codec.startswith('mlvc') else ['native']
    return ['bicubic', 'lanczos', 'sr']


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


def render(report, output, html_only=False):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import ScalarFormatter

    output.mkdir(parents=True, exist_ok=True)
    c = report['config']
    size = f"{c['width']}×{c['height']}"
    coded_size = f"{c['width']//c['scale']}×{c['height']//c['scale']}"
    panel_labels = dict(PANEL_LABELS, native=f'{size} 编码参照',
                        low_native=f'{coded_size} 编码 RD', delivery=f'{size} 输出对比',
                        low=f'{coded_size} → {size} 重建')
    (output / 'rd.json').write_text(json.dumps(report, indent=2, ensure_ascii=False,
                                             allow_nan=False) + '\n', encoding='utf-8')
    plt.rcParams.update({'font.family': 'sans-serif',
                        'font.sans-serif': ['Microsoft YaHei', 'Noto Sans CJK SC', 'SimHei',
                                            'WenQuanYi Zen Hei', 'DejaVu Sans'],
                        'axes.unicode_minus': False, 'font.size': 11,
                        'axes.spines.top': False, 'axes.spines.right': False,
                        'axes.labelcolor': '#526070', 'text.color': '#182535',
                        'axes.edgecolor': '#cdd6de', 'xtick.color': '#526070',
                        'ytick.color': '#526070', 'figure.facecolor': '#f3f6fa',
                        'axes.facecolor': '#ffffff', 'savefig.facecolor': '#ffffff'})
    configured_sequences = [s['name'] for s in report['config']['sequences']]
    measured_sequences = {p['sequence'] for p in report['points'] if p['status'] == 'ok'}
    sequences = [s for s in configured_sequences if s in measured_sequences] or configured_sequences[:1]
    pooled = aggregate(report['points'], configured_sequences) if len(configured_sequences) > 1 else []
    data = groups(report['points'] + pooled)
    if pooled:
        sequences = ['Suite aggregate'] + sequences
    images = []
    for index, seq in enumerate(sequences):
        # Separate the codec overview from SR ablations to avoid a 16-line legend.
        panels = [('native', panel_labels['native'], list(COLORS)),
                  ('low_native', panel_labels['low_native'], list(COLORS)),
                  ('delivery', panel_labels['delivery'], list(COLORS))] + [
            ('low', LABELS[codec] + f' · {coded_size} → {size}', [codec]) for codec in COLORS]
        panels = [p for p in panels
                  if any(data.get((seq, codec, m)) for codec in p[2] for m in panel_methods(p[0], codec))]
        if html_only:
            for branch, title, codecs in panels:
                name = f'{index:02d}-{branch}-' + ('codecs' if branch in ('native', 'low_native', 'delivery') else codecs[0])
                images.append((seq, branch, name + '.png'))
            if any(k[0] == seq and k[2] != 'low_native' for k in data):
                images.append((seq, 'performance', f'{index:02d}-performance.png'))
            continue
        for branch, title, codecs in panels:
            fig, axes = plt.subplots(1, 2, figsize=(12.8, 5.2), layout='constrained')
            fig.suptitle(f'{sequence_label(seq)}  /  {title}', fontsize=18, fontweight='bold')
            for ax, metric, label in zip(axes, ('psnr_y', 'ssim_y'), ('Y-PSNR (dB)', 'Y-SSIM')):
                count = 0
                for codec in codecs:
                    for method in panel_methods(branch, codec):
                        rows = sorted(data.get((seq, codec, method), []), key=lambda r: r['kbps'])
                        rows = [r for r in rows if r.get(metric) is not None and math.isfinite(r[metric])]
                        if not rows:
                            continue
                        style, marker = STYLES[method]
                        ax.plot([r['kbps'] for r in rows], [r[metric] for r in rows],
                                linestyle=style, marker=marker, color=COLORS[codec],
                                linewidth=2.3, markersize=6,
                                alpha=1 if method in ('native', 'sr') else .65,
                                label=(f'MLVC + {METHOD_LABELS[method]}' if branch == 'delivery' and codec.startswith('mlvc') else
                                      LABELS[codec]) if method in ('native', 'low_native') or branch == 'delivery' else
                                      ('SR 超分辨率 · ' + report['config']['sr_model'] if method == 'sr' else METHOD_LABELS[method]))
                        count += 1
                ax.set(xscale='log', xlabel='实际码流码率 (kbps)', ylabel=label)
                ax.xaxis.set_major_formatter(ScalarFormatter())
                ax.grid(True, alpha=.22, linestyle='--', which='both')
                if count:
                    ax.legend(loc='best', frameon=False, fontsize=9)
                else:
                    ax.text(.5, .5, '暂无有效测量结果', ha='center', va='center', transform=ax.transAxes)
            name = f'{index:02d}-{branch}-' + ('codecs' if branch in ('native', 'low_native', 'delivery') else codecs[0])
            for extension in ('png', 'svg'):
                fig.savefig(output / f'{name}.{extension}', dpi=180)
            plt.close(fig)
            images.append((seq, branch, name + '.png'))

        perf = [(k, rows) for k, rows in data.items() if k[0] == seq and k[2] != 'low_native']
        if perf:
            fig, axes = plt.subplots(1, 2, figsize=(12.8, 5.5), layout='constrained')
            fig.suptitle(f'{sequence_label(seq)}  /  吞吐与码率', fontsize=18, fontweight='bold')
            for ax, metric, label in zip(axes, ('stage_sum_fps', 'realtime_speed'),
                                         ('分阶段耗时之和对应吞吐 (fps)', '实时倍速 (×，越高越快)')):
                for (s, codec, method), rows in perf:
                    rows = sorted(rows, key=lambda r: r['kbps'])
                    style, marker = STYLES[method]
                    ax.plot([r['kbps'] for r in rows], [r[metric] for r in rows],
                            color=COLORS[codec], linestyle=style, marker=marker, markersize=4,
                            label=f'{codec} / {METHOD_LABELS[method]}')
                ax.set(xscale='log', yscale='log', xlabel='实际码流码率 (kbps)', ylabel=label)
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
    body = ''.join(f'<figure data-sequence="{esc(seq)}" data-panel="{branch}"><img loading="lazy" src="{name}" alt="{esc(sequence_label(seq))} {panel_labels[branch]}"><figcaption><span>{esc(sequence_label(seq))} · {panel_labels[branch]}</span><span><a href="{name[:-4]}.svg">SVG 矢量图 ↗</a><a href="{name}">PNG 图片 ↗</a></span></figcaption></figure>' for seq, branch, name in images)
    options = ''.join(f'<option value="{esc(seq)}">{esc(sequence_label(seq))}</option>' for seq in sequences)
    field_labels = {'reconstruction': METHOD_LABELS, 'branch': panel_labels}
    failures = ''.join('<tr>' + ''.join(f'<td>{esc(field_labels.get(k, {}).get(p.get(k), p.get(k, "—")))}</td>' for k in
                     ('sequence', 'codec', 'qp', 'branch', 'reconstruction', 'error')) + '</tr>' for p in failed)
    recon = c.get('reconstructions', ['low_native', 'bicubic', 'lanczos', 'sr'])
    native = 0 if c.get('native_reference', True) is False else 1
    expected = len(report['config']['sequences']) * sum(
        len(qps) * (len(recon) if codec.startswith('mlvc') else len(recon) + native)
        for codec, qps in report['config']['codecs'].items())
    valid = sum(p['status'] == 'ok' for p in report['points'])
    state = '测量未完成' if failed or report.get('fatal_error') or valid != expected else '实测完成'
    stats_data = [('有效测量点', f'{valid} / {expected}', '全部序列 × QP × 重建路径'),
                  ('编码 RD 对比尺寸', coded_size, f'输出评价 {size}'),
                  ('已覆盖测试序列', f"{len(measured_sequences)} / {len(c['sequences'])}", f"每序列 {c['frames']} 帧 · {c['fps']} fps"),
                  ('关键帧周期', f"{c['gop']} 帧" if 'gop' in c else '后端默认',
                   '逐码流核验' if 'gop' in c else '旧版测量：未统一 GOP')]
    stats = ''.join(f'<div class="stat"><small>{title}</small><strong>{value}</strong><span>{note}</span></div>'
                    for title, value, note in stats_data)
    active_views = {branch for _, branch, _ in images}
    views = ''.join(f'<button type="button" data-view="{key}" aria-pressed="false">{value}</button>'
                    for key, value in list(panel_labels.items()) if key in active_views)
    views += '<button type="button" data-view="all" aria-pressed="false">全部对比</button>'
    qp_rows = ''.join(f'<tr><td>{LABELS[codec]}</td><td>{", ".join(map(str, qps))}</td><td>{len(qps)} 档</td></tr>'
                      for codec, qps in c['codecs'].items())
    audited = all((p.get('gop_audit') or {}).get('verified') for p in report['points']
                  if p['status'] == 'ok') if valid else False
    align = c.get('mlvc_alignment', 16)
    mw = (c['width']//c['scale'] + align - 1)//align*align
    mh = (c['height']//c['scale'] + align - 1)//align*align
    protocol = (f"{coded_size} 编码 RD 的输入与评价尺寸一致；MLVC 补边编码为 {mw}×{mh}，解码后裁回 {coded_size}。")
    if any(m != 'low_native' for m in recon):
        protocol += f"重建视图统一对原始 {size} 参考评分。"
    if 'gop' in c:
        protocol += f"统一编码帧率 {c['fps']} fps、关键帧周期 {c['gop']} 帧，仅使用过去帧参考，MLVC 长时参考关闭。"
        protocol += '有效点的关键帧位置已从码流核验。' if audited else '请检查各点的关键帧核验记录。'
    else:
        protocol += '此报告为旧版实测，各后端 GOP / FPS 使用各自默认值，不能视为统一条件排名。'
    sampling = f"每点采样 {c['iterations']} 次，预热 {c['warmup']} 次。"
    if c['iterations'] == 1:
        sampling += '本次计时为单次样本，仅供探索性性能参考。'
    page = Template(Path(__file__).with_name('report.html').read_text(encoding='utf-8')).substitute(
        state=state, state_class='' if state == '实测完成' else 'pending',
        board=esc(report['system'].get('hostname', '未记录板卡')), timestamp=esc(report['system'].get('timestamp_utc', '')),
        summary=f"从 {coded_size} 编码到 {size} 重建，对照 MLVC、H.264、H.265 与 AV1 的码率、画质和板端吞吐。",
        stats=stats, views=views, options=options, protocol=protocol, body=body,
        qp_rows=qp_rows, sampling=sampling, failure_open='open' if failed else '', failed_count=len(failed),
        fatal=esc(report.get('fatal_error', '无缺失或失败点。' if not failed else '详见下表。')),
        failures=failures, revision=esc(c.get('source_revision', '未记录')),
        provenance=esc(json.dumps({k: v for k, v in report.items() if k not in ('points', 'commands')}, indent=2, ensure_ascii=False)))
    (output / 'index.html').write_text(page, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--html-only', action='store_true', help='reuse existing plots; update the HTML page only')
    args = parser.parse_args()
    render(json.loads(args.report.read_text(encoding='utf-8')), args.output, args.html_only)
