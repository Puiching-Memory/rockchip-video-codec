"""Tests for the stdlib-only rkvc benchmark runner."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from tools.bench import benchmark


class BenchmarkCaseTests(unittest.TestCase):
    def test_decode_dimensions_are_metrics_only(self) -> None:
        case = benchmark.BenchmarkCase(
            name="decode",
            operation="decode",
            input_path=pathlib.Path("input.h264"),
            output_name="output.nv12",
            codec="h264",
            width=1920,
            height=1080,
        )
        command = case.command("rkvc", pathlib.Path("output.nv12"))
        self.assertNotIn("--width", command)
        self.assertNotIn("--height", command)

    def test_encode_infers_nv12_frame_count_and_builds_command(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            # 4x2 NV12 = 12 bytes per frame.
            (root / "input.nv12").write_bytes(bytes(24))
            case = benchmark.BenchmarkCase.from_mapping(
                {
                    "name": "tiny-encode",
                    "operation": "encode",
                    "input": "input.nv12",
                    "output": "encoded.h264",
                    "codec": "h264",
                    "width": 4,
                    "height": 2,
                    "bitrate_bps": 1000,
                    "qp": 24,
                },
                root,
            )

            self.assertEqual(case.frames, 2)
            self.assertEqual(
                case.command("rkvc", root / "out.h264"),
                [
                    "rkvc",
                    "encode",
                    "-i",
                    str(root / "input.nv12"),
                    "-o",
                    str(root / "out.h264"),
                    "--codec",
                    "h264",
                    "--width",
                    "4",
                    "--height",
                    "2",
                    "--bitrate",
                    "1000",
                    "--qp",
                    "24",
                ],
            )

    def test_rejects_reserved_extra_arguments(self) -> None:
        with self.assertRaisesRegex(benchmark.BenchmarkError, "extra_args"):
            benchmark.BenchmarkCase.from_mapping(
                {
                    "name": "bad",
                    "operation": "decode",
                    "input": "input.h264",
                    "extra_args": ["-o", "somewhere"],
                },
                pathlib.Path.cwd(),
            )


class ConfigAndStatisticsTests(unittest.TestCase):
    def test_config_paths_are_relative_to_config(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            config_path = root / "bench.json"
            config_path.write_text(
                json.dumps(
                    {
                        "rkvc": "bin/rkvc",
                        "warmup": 0,
                        "iterations": 3,
                        "cases": [
                            {
                                "name": "decode",
                                "operation": "decode",
                                "input": "media/in.h264",
                                "codec": "h264",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            config = benchmark.load_config(config_path)

            self.assertEqual(config.rkvc, str(root / "bin" / "rkvc"))
            self.assertEqual(config.cases[0].input_path, root / "media" / "in.h264")
            self.assertEqual(config.warmup, 0)
            self.assertEqual(config.iterations, 3)

    def test_summary_uses_interpolated_p95(self) -> None:
        summary = benchmark.summarize([1.0, 2.0, 3.0, 4.0])
        self.assertEqual(summary["mean"], 2.5)
        self.assertEqual(summary["median"], 2.5)
        self.assertAlmostEqual(summary["p95"], 3.85)

    def test_threshold_failure_uses_mean(self) -> None:
        case = benchmark.BenchmarkCase(
            name="threshold",
            operation="decode",
            input_path=pathlib.Path("input.h264"),
            output_name="output.nv12",
            thresholds=benchmark.Thresholds(min_fps=60.0),
        )
        failures = benchmark._threshold_failures(  # pylint: disable=protected-access
            case, {"fps": benchmark.summarize([58.0, 60.0])}
        )
        self.assertEqual(len(failures), 1)


if __name__ == "__main__":
    unittest.main()
