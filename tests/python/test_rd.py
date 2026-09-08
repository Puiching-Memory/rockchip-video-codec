"""RD accounting and artifact validation independent of Rockchip hardware."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from tools.bench import rd
from tools.bench.plot_rd import groups, aggregate

ROOT = Path(__file__).resolve().parents[2]
(ROOT / '.temp').mkdir(exist_ok=True)


class RDTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads((ROOT / 'tools/bench/rd.uvg.json').read_text())

    def test_geometry_and_rate_validation(self):
        rd.validate(self.config)
        for key, value in [('fps', float('nan')), ('scale', 7), ('frames', True)]:
            c = copy.deepcopy(self.config)
            c[key] = value
            with self.assertRaises(ValueError):
                rd.validate(c)

    def test_vendor_banner_does_not_hide_registry_json(self):
        self.assertEqual(rd.model_listing('rga_api version 1.10.6\n{"models": []}\n'), [])
        with self.assertRaises(ValueError):
            rd.model_listing('rga_api version 1.10.6\n')

    def test_reject_truncated_reconstruction(self):
        with tempfile.TemporaryDirectory(dir=ROOT / '.temp') as tmp:
            p = Path(tmp) / 'rec.nv12'
            p.write_bytes(bytes(11))
            self.config['frames'] = 1
            runner = rd.Runner(self.config, Path(tmp))
            with self.assertRaisesRegex(ValueError, 'exact frame count'):
                runner.check_raw(p, 4, 2)

    def test_metrics_use_pooled_mse_and_exact_frames(self):
        with tempfile.TemporaryDirectory(dir=ROOT / '.temp') as tmp:
            work = Path(tmp)
            self.config.update(frames=2, width=4, height=2)
            rec = work / 'rec.nv12'
            rec.write_bytes(bytes(24))
            (work / 'psnr.txt').write_text('n:1 mse_y:1.00\nn:2 mse_y:9.00\n')
            (work / 'ssim.txt').write_text('n:1 Y:0.90\nn:2 Y:0.80\n')
            runner = rd.Runner(self.config, work)
            runner.run = lambda args: (1, '', '')
            scores = runner.quality(rec, rec, work)
            self.assertAlmostEqual(scores['psnr_y'], 41.1411035653)
            self.assertAlmostEqual(scores['ssim_y'], .85)
            (work / 'ssim.txt').write_text('n:1 Y:0.90\n')
            with self.assertRaisesRegex(ValueError, 'incomplete frame pairing'):
                runner.quality(rec, rec, work)

    def test_shared_stream_rate_and_missing_sr(self):
        with tempfile.TemporaryDirectory(dir=ROOT / '.temp') as tmp:
            work = Path(tmp)
            self.config.update(frames=2, width=12, height=6, fps=30, warmup=0, iterations=1,
                               classical_decoder='rkvc')
            runner = rd.Runner(self.config, work)
            def fake_run(args):
                if 'encode' in args:
                    Path(args[args.index('-o') + 1]).write_bytes(bytes(1000))
                elif 'decode' in args:
                    Path(args[args.index('-o') + 1]).write_bytes(bytes(24))
                else:
                    Path(args[-1]).write_bytes(bytes(216))
                return (1, '', '')
            runner.run = fake_run
            runner.audit_gop = lambda *args: None
            runner.quality = lambda *args: {'psnr_y': 30, 'ssim_y': .9}
            rows = runner.point('h264', 22, 'low', work/'ref', work/'low', .5, work, False)
            self.assertEqual([r['kbps'] for r in rows], [120, 120, 120, 120])
            self.assertEqual([r['status'] for r in rows], ['ok', 'ok', 'ok', 'failed'])
            self.assertAlmostEqual(rows[1]['stage_sum_seconds'], 3.5)
            self.assertEqual(len(groups([dict(r, sequence='test') for r in rows])), 3)

    def test_aggregate_requires_complete_suite_and_pools_mse(self):
        common = dict(codec='h264', qp=22, reconstruction='native', status='ok',
                      kbps=100, bpp=.1, ssim_y=.9, stage_sum_fps=30, realtime_speed=1)
        rows = [dict(common, sequence='a', psnr_y=30), dict(common, sequence='b', psnr_y=40)]
        self.assertEqual(aggregate(rows[:1], ['a', 'b']), [])
        pooled = aggregate(rows, ['a', 'b'])[0]
        self.assertAlmostEqual(pooled['psnr_y'], 32.5963731051)
        self.assertNotEqual(pooled['psnr_y'], 35)

    def test_resume_retries_a_partially_measured_stream(self):
        common = dict(sequence='Beauty', codec='mlvc', qp=21, branch='low', status='ok')
        rows = [dict(common, reconstruction=m) for m in ('low_native', 'bicubic', 'lanczos')]
        self.assertEqual(rd.completed_points(rows), set())
        rows.append(dict(common, reconstruction='sr', status='failed'))
        self.assertEqual(rd.completed_points(rows), set())
        rows[-1]['status'] = 'ok'
        self.assertEqual(rd.completed_points(rows), {('Beauty', 'mlvc', 'low', 21)})

    def test_resume_rejects_model_or_board_changes(self):
        before = dict(config=self.config, rkvc_sha256='binary', library_hashes={'lib': 'a'},
                      model_hashes={'sr': 'a'}, ffmpeg_version='version', cpu_affinity=[4, 5],
                      system={'hostname': 'RK3576', 'machine': 'aarch64'})
        rd.verify_resume(before, copy.deepcopy(before))
        after = copy.deepcopy(before)
        after['model_hashes']['sr'] = 'different-model'
        with self.assertRaisesRegex(ValueError, 'model_hashes changed'):
            rd.verify_resume(before, after)
        after = copy.deepcopy(before)
        after['system']['hostname'] = 'another-board'
        with self.assertRaisesRegex(ValueError, 'hostname changed'):
            rd.verify_resume(before, after)

    def test_explicit_encoder_controls_and_keyframe_audit(self):
        self.config.update(gop=64, frames=96)
        runner = rd.Runner(self.config, ROOT / '.temp')
        for codec in ('h264', 'hevc', 'av1', 'mlvc'):
            args = runner.media('encode', 'in', 'out', codec)
            self.assertEqual(args[args.index('--gop') + 1], 64)
            self.assertEqual(args[args.index('--fps') + 1], 120)
            self.assertIn('--low-delay', args)
        def log(interval):
            return json.dumps({'frames': [{'key_frame': int(i % interval == 0), 'pict_type': 'P'} for i in range(96)]})
        runner.run = lambda *args: (0, log(64), '')
        self.assertEqual(runner.audit_gop(Path('stream'), 'h264')['keyframes'], [0, 64])
        runner.run = lambda *args: (0, log(60), '')
        with self.assertRaisesRegex(ValueError, 'GOP audit failed'):
            runner.audit_gop(Path('stream'), 'h264')
        runner.run = lambda *args: (0, '', '\n'.join(
            f'frame_type 00 = {int(i % 64 != 0)}' for i in range(96)))
        self.assertEqual(runner.audit_gop(Path('stream'), 'av1')['keyframes'], [0, 64])


if __name__ == '__main__':
    unittest.main()
