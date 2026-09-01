#!/usr/bin/env python3
"""Repeatable end-to-end performance benchmark for the rkvc CLI.

The runner deliberately depends only on the Python standard library so it can
be copied to a Rockchip target together with a portable rkvc package.
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import datetime as dt
import json
import math
import os
import pathlib
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterable, Mapping, Sequence


SCHEMA_VERSION = 1
OPERATIONS = {"decode", "encode", "transcode"}
CODECS = {"auto", "h264", "hevc", "av1"}


class BenchmarkError(RuntimeError):
    """A configuration or benchmark execution error."""


@dataclasses.dataclass(frozen=True)
class Thresholds:
    min_fps: float | None = None
    min_realtime: float | None = None
    max_mean_seconds: float | None = None


@dataclasses.dataclass(frozen=True)
class BenchmarkCase:
    name: str
    operation: str
    input_path: pathlib.Path
    output_name: str
    codec: str = "auto"
    width: int | None = None
    height: int | None = None
    bitrate_bps: int | None = None
    qp: int | None = None
    frames: int | None = None
    duration_seconds: float | None = None
    extra_args: tuple[str, ...] = ()
    env: Mapping[str, str] = dataclasses.field(default_factory=dict)
    thresholds: Thresholds = Thresholds()

    @classmethod
    def from_mapping(
        cls, raw: Mapping[str, Any], base_dir: pathlib.Path
    ) -> "BenchmarkCase":
        if not isinstance(raw, Mapping):
            raise BenchmarkError("each benchmark case must be a JSON object")

        name = _required_text(raw, "name")
        if name in {".", ".."} or "/" in name or "\\" in name:
            raise BenchmarkError(f"case {name!r}: name must not contain path separators")
        operation = _required_text(raw, "operation").lower()
        if operation not in OPERATIONS:
            raise BenchmarkError(
                f"case {name!r}: operation must be one of {sorted(OPERATIONS)}"
            )

        input_text = _required_text(raw, "input")
        input_path = pathlib.Path(input_text).expanduser()
        if not input_path.is_absolute():
            input_path = (base_dir / input_path).resolve()

        codec = str(raw.get("codec", "auto")).lower()
        if codec not in CODECS:
            raise BenchmarkError(
                f"case {name!r}: codec must be one of {sorted(CODECS)}"
            )
        if operation in {"encode", "transcode"} and codec == "auto":
            raise BenchmarkError(f"case {name!r}: {operation} requires codec")

        width = _optional_positive_int(raw, "width")
        height = _optional_positive_int(raw, "height")
        if operation == "encode" and (width is None or height is None):
            raise BenchmarkError(f"case {name!r}: encode requires width and height")

        output_name = str(raw.get("output", _default_output(operation, codec)))
        if "/" in output_name or "\\" in output_name:
            raise BenchmarkError(
                f"case {name!r}: output must be a filename, not a path"
            )
        output_name = pathlib.Path(output_name).name
        if not output_name or output_name in {".", ".."}:
            raise BenchmarkError(f"case {name!r}: invalid output filename")

        extra = raw.get("extra_args", [])
        if not isinstance(extra, list) or not all(isinstance(v, str) for v in extra):
            raise BenchmarkError(f"case {name!r}: extra_args must be a string array")
        reserved = {"-i", "--input", "-o", "--output"}
        if reserved.intersection(extra):
            raise BenchmarkError(
                f"case {name!r}: input/output flags are not allowed in extra_args"
            )

        env = raw.get("env", {})
        if not isinstance(env, Mapping) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in env.items()
        ):
            raise BenchmarkError(f"case {name!r}: env must map strings to strings")

        thresholds_raw = raw.get("thresholds", {})
        if not isinstance(thresholds_raw, Mapping):
            raise BenchmarkError(f"case {name!r}: thresholds must be an object")
        thresholds = Thresholds(
            min_fps=_optional_positive_float(thresholds_raw, "min_fps"),
            min_realtime=_optional_positive_float(
                thresholds_raw, "min_realtime"
            ),
            max_mean_seconds=_optional_positive_float(
                thresholds_raw, "max_mean_seconds"
            ),
        )

        case = cls(
            name=name,
            operation=operation,
            input_path=input_path,
            output_name=output_name,
            codec=codec,
            width=width,
            height=height,
            bitrate_bps=_optional_positive_int(raw, "bitrate_bps"),
            qp=_optional_nonnegative_int(raw, "qp"),
            frames=_optional_positive_int(raw, "frames"),
            duration_seconds=_optional_positive_float(raw, "duration_seconds"),
            extra_args=tuple(extra),
            env=dict(env),
            thresholds=thresholds,
        )
        return dataclasses.replace(case, frames=case.frames or case.infer_frames())

    def infer_frames(self) -> int | None:
        """Infer NV12 frame count for encode cases from input size."""
        if self.operation != "encode" or not self.width or not self.height:
            return None
        if not self.input_path.is_file():
            return None
        frame_bytes = self.width * self.height * 3 // 2
        size = self.input_path.stat().st_size
        if frame_bytes > 0 and size > 0 and size % frame_bytes == 0:
            return size // frame_bytes
        return None

    def command(self, rkvc: str, output_path: pathlib.Path) -> list[str]:
        command = [
            rkvc,
            self.operation,
            "-i",
            str(self.input_path),
            "-o",
            str(output_path),
        ]
        if self.codec != "auto" or self.operation != "decode":
            command.extend(("--codec", self.codec))
        # Decode width/height describe the source for MP/s reporting only.  The
        # decode CLI output size must follow the bitstream instead of turning a
        # throughput test into an implicit scale operation.
        if self.width is not None and self.operation != "decode":
            command.extend(("--width", str(self.width)))
        if self.height is not None and self.operation != "decode":
            command.extend(("--height", str(self.height)))
        if self.bitrate_bps is not None:
            command.extend(("--bitrate", str(self.bitrate_bps)))
        if self.qp is not None:
            command.extend(("--qp", str(self.qp)))
        command.extend(self.extra_args)
        return command


@dataclasses.dataclass(frozen=True)
class RunnerConfig:
    rkvc: str
    warmup: int
    iterations: int
    timeout_seconds: float
    cooldown_seconds: float
    cases: tuple[BenchmarkCase, ...]


def _required_text(raw: Mapping[str, Any], key: str) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value.strip():
        raise BenchmarkError(f"{key} must be a non-empty string")
    return value.strip()


def _optional_positive_int(raw: Mapping[str, Any], key: str) -> int | None:
    value = raw.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise BenchmarkError(f"{key} must be a positive integer")
    return value


def _optional_nonnegative_int(raw: Mapping[str, Any], key: str) -> int | None:
    value = raw.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise BenchmarkError(f"{key} must be a non-negative integer")
    return value


def _optional_positive_float(raw: Mapping[str, Any], key: str) -> float | None:
    value = raw.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        raise BenchmarkError(f"{key} must be a positive number")
    return float(value)


def _default_output(operation: str, codec: str) -> str:
    if operation == "decode":
        return "output.nv12"
    return {
        "h264": "output.h264",
        "hevc": "output.h265",
        "av1": "output.ivf",
    }.get(codec, "output.es")


def load_config(path: pathlib.Path) -> RunnerConfig:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BenchmarkError(f"cannot read config {path}: {exc}") from exc
    if not isinstance(raw, Mapping):
        raise BenchmarkError("benchmark config must be a JSON object")
    cases_raw = raw.get("cases")
    if not isinstance(cases_raw, list) or not cases_raw:
        raise BenchmarkError("benchmark config requires a non-empty cases array")

    rkvc = str(raw.get("rkvc", "../../.build/release/rkvc"))
    rkvc_path = pathlib.Path(rkvc).expanduser()
    if ("/" in rkvc or "\\" in rkvc) and not rkvc_path.is_absolute():
        rkvc = str((path.parent / rkvc_path).resolve())

    return RunnerConfig(
        rkvc=rkvc,
        warmup=_nonnegative_int(raw, "warmup", 1),
        iterations=_positive_int(raw, "iterations", 5),
        timeout_seconds=_positive_float(raw, "timeout_seconds", 600.0),
        cooldown_seconds=_nonnegative_float(raw, "cooldown_seconds", 0.0),
        cases=tuple(BenchmarkCase.from_mapping(case, path.parent) for case in cases_raw),
    )


def _positive_int(raw: Mapping[str, Any], key: str, default: int) -> int:
    value = raw.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise BenchmarkError(f"{key} must be a positive integer")
    return value


def _nonnegative_int(raw: Mapping[str, Any], key: str, default: int) -> int:
    value = raw.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise BenchmarkError(f"{key} must be a non-negative integer")
    return value


def _positive_float(raw: Mapping[str, Any], key: str, default: float) -> float:
    value = raw.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        raise BenchmarkError(f"{key} must be a positive number")
    return float(value)


def _nonnegative_float(raw: Mapping[str, Any], key: str, default: float) -> float:
    value = raw.get(key, default)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
        raise BenchmarkError(f"{key} must be a non-negative number")
    return float(value)


def percentile(values: Sequence[float], fraction: float) -> float:
    if not values:
        raise BenchmarkError("cannot calculate a percentile of no samples")
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def summarize(values: Sequence[float]) -> dict[str, float]:
    if not values:
        raise BenchmarkError("cannot summarize no samples")
    mean = statistics.fmean(values)
    return {
        "mean": mean,
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "p95": percentile(values, 0.95),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def _sample_metrics(case: BenchmarkCase, elapsed: float, output_size: int) -> dict[str, Any]:
    input_size = case.input_path.stat().st_size
    result: dict[str, Any] = {
        "elapsed_seconds": elapsed,
        "input_bytes": input_size,
        "output_bytes": output_size,
        "input_mbps": input_size * 8.0 / elapsed / 1_000_000.0,
        "output_mbps": output_size * 8.0 / elapsed / 1_000_000.0,
    }
    if case.frames:
        result["fps"] = case.frames / elapsed
        if case.width and case.height:
            result["megapixels_per_second"] = (
                case.frames * case.width * case.height / elapsed / 1_000_000.0
            )
    if case.duration_seconds:
        result["realtime"] = case.duration_seconds / elapsed
    return result


def _run_once(
    rkvc: str,
    case: BenchmarkCase,
    output_path: pathlib.Path,
    timeout_seconds: float,
) -> tuple[dict[str, Any], str, str]:
    command = case.command(rkvc, output_path)
    env = os.environ.copy()
    env.update(case.env)
    start = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            timeout=timeout_seconds,
            env=env,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise BenchmarkError(
            f"case {case.name!r} timed out after {timeout_seconds:g}s"
        ) from exc
    except OSError as exc:
        raise BenchmarkError(f"cannot execute {rkvc!r}: {exc}") from exc
    elapsed = time.perf_counter() - start
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        if len(detail) > 2000:
            detail = detail[-2000:]
        raise BenchmarkError(
            f"case {case.name!r} exited with {completed.returncode}: {detail}"
        )
    output_size = output_path.stat().st_size if output_path.is_file() else 0
    return (
        _sample_metrics(case, max(elapsed, sys.float_info.epsilon), output_size),
        completed.stdout,
        completed.stderr,
    )


def _case_summary(samples: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    metric_names = (
        "elapsed_seconds",
        "fps",
        "realtime",
        "megapixels_per_second",
        "input_mbps",
        "output_mbps",
    )
    return {
        name: summarize([float(sample[name]) for sample in samples if name in sample])
        for name in metric_names
        if any(name in sample for sample in samples)
    }


def _threshold_failures(
    case: BenchmarkCase, summary: Mapping[str, Mapping[str, float]]
) -> list[str]:
    failures: list[str] = []
    checks = (
        ("fps", case.thresholds.min_fps, "min_fps", lambda actual, limit: actual < limit),
        (
            "realtime",
            case.thresholds.min_realtime,
            "min_realtime",
            lambda actual, limit: actual < limit,
        ),
        (
            "elapsed_seconds",
            case.thresholds.max_mean_seconds,
            "max_mean_seconds",
            lambda actual, limit: actual > limit,
        ),
    )
    for metric, limit, label, failed in checks:
        if limit is None:
            continue
        if metric not in summary:
            failures.append(f"{label} configured but {metric} cannot be calculated")
            continue
        actual = summary[metric]["mean"]
        if failed(actual, limit):
            failures.append(f"{label}={limit:g}, measured mean={actual:.3f}")
    return failures


def run_case(
    rkvc: str,
    case: BenchmarkCase,
    warmup: int,
    iterations: int,
    timeout_seconds: float,
    cooldown_seconds: float,
    work_dir: pathlib.Path,
    quiet: bool = False,
) -> dict[str, Any]:
    if not case.input_path.is_file():
        raise BenchmarkError(f"case {case.name!r}: input not found: {case.input_path}")
    output_path = work_dir / case.output_name
    if not quiet:
        print(f"\n[{case.name}] {shlex.join(case.command(rkvc, output_path))}")

    for index in range(warmup):
        if output_path.exists():
            output_path.unlink()
        if not quiet:
            print(f"  warmup {index + 1}/{warmup}", end="", flush=True)
        _run_once(rkvc, case, output_path, timeout_seconds)
        if not quiet:
            print(" done")
        if cooldown_seconds:
            time.sleep(cooldown_seconds)

    samples: list[dict[str, Any]] = []
    for index in range(iterations):
        if output_path.exists():
            output_path.unlink()
        sample, _, _ = _run_once(rkvc, case, output_path, timeout_seconds)
        sample["iteration"] = index + 1
        samples.append(sample)
        if not quiet:
            metrics = [f"{sample['elapsed_seconds']:.3f}s"]
            if "fps" in sample:
                metrics.append(f"{sample['fps']:.2f} fps")
            if "realtime" in sample:
                metrics.append(f"{sample['realtime']:.2f}x realtime")
            print(f"  run {index + 1}/{iterations}: " + ", ".join(metrics))
        if cooldown_seconds and index + 1 < iterations:
            time.sleep(cooldown_seconds)

    summary = _case_summary(samples)
    failures = _threshold_failures(case, summary)
    return {
        "name": case.name,
        "operation": case.operation,
        "codec": case.codec,
        "input": str(case.input_path),
        "frames": case.frames,
        "duration_seconds": case.duration_seconds,
        "width": case.width,
        "height": case.height,
        "command": case.command(rkvc, output_path),
        "samples": samples,
        "summary": summary,
        "threshold_failures": failures,
        "status": "failed" if failures else "passed",
    }


def _git_revision() -> str | None:
    root = pathlib.Path(__file__).resolve().parents[2]
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def system_metadata() -> dict[str, Any]:
    metadata: dict[str, Any] = {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "hostname": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
        "git_revision": _git_revision(),
    }
    if sys.platform.startswith("linux"):
        metadata.update(_linux_metadata())
    return metadata


def _linux_metadata() -> dict[str, Any]:
    """Collect best-effort board state useful when comparing performance."""
    thermal: list[dict[str, Any]] = []
    for zone in sorted(pathlib.Path("/sys/class/thermal").glob("thermal_zone*")):
        try:
            zone_type = (zone / "type").read_text(encoding="utf-8").strip()
            temperature = float((zone / "temp").read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            continue
        thermal.append(
            {
                "zone": zone.name,
                "type": zone_type,
                "temperature_c": temperature / 1000.0,
            }
        )

    governors: set[str] = set()
    for path in pathlib.Path("/sys/devices/system/cpu").glob(
        "cpu[0-9]*/cpufreq/scaling_governor"
    ):
        try:
            governors.add(path.read_text(encoding="utf-8").strip())
        except OSError:
            pass

    compatible: list[str] = []
    try:
        compatible = [
            value.decode("ascii", errors="replace")
            for value in pathlib.Path("/proc/device-tree/compatible")
            .read_bytes()
            .split(b"\0")
            if value
        ]
    except OSError:
        pass

    try:
        load_average: list[float] | None = list(os.getloadavg())
    except OSError:
        load_average = None
    return {
        "device_tree_compatible": compatible,
        "cpu_governors": sorted(governors),
        "load_average": load_average,
        "thermal": thermal,
    }


def write_artifacts(report: Mapping[str, Any], results_dir: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    results_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    json_path = results_dir / f"benchmark-{stamp}.json"
    csv_path = results_dir / f"benchmark-{stamp}.csv"
    json_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    fields = [
        "case",
        "operation",
        "codec",
        "iteration",
        "elapsed_seconds",
        "fps",
        "realtime",
        "megapixels_per_second",
        "input_mbps",
        "output_mbps",
        "input_bytes",
        "output_bytes",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for case in report["cases"]:
            for sample in case["samples"]:
                writer.writerow(
                    {
                        "case": case["name"],
                        "operation": case["operation"],
                        "codec": case["codec"],
                        **sample,
                    }
                )
    return json_path, csv_path


def _print_summary(report: Mapping[str, Any]) -> None:
    print("\nSummary")
    print("-------")
    for case in report["cases"]:
        summary = case["summary"]
        parts = [f"mean {summary['elapsed_seconds']['mean']:.3f}s"]
        if "fps" in summary:
            parts.append(f"{summary['fps']['mean']:.2f} fps")
        if "realtime" in summary:
            parts.append(f"{summary['realtime']['mean']:.2f}x realtime")
        parts.append(f"p95 {summary['elapsed_seconds']['p95']:.3f}s")
        print(f"{case['name']}: " + ", ".join(parts) + f" [{case['status']}]")
        for failure in case["threshold_failures"]:
            print(f"  threshold: {failure}")


def _direct_config(args: argparse.Namespace) -> RunnerConfig:
    if not args.operation or not args.input:
        raise BenchmarkError("use --config, or provide --operation and --input")
    raw: dict[str, Any] = {
        "name": args.name or f"{args.operation}-{args.codec}",
        "operation": args.operation,
        "input": args.input,
        "codec": args.codec,
    }
    for key in ("output", "width", "height", "bitrate_bps", "qp", "frames", "duration_seconds"):
        value = getattr(args, key)
        if value is not None:
            raw[key] = value
    return RunnerConfig(
        rkvc=args.rkvc or str(pathlib.Path(".build/release/rkvc")),
        warmup=args.warmup if args.warmup is not None else 1,
        iterations=args.iterations if args.iterations is not None else 5,
        timeout_seconds=args.timeout if args.timeout is not None else 600.0,
        cooldown_seconds=args.cooldown if args.cooldown is not None else 0.0,
        cases=(BenchmarkCase.from_mapping(raw, pathlib.Path.cwd()),),
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark rkvc decode/encode/transcode throughput with warmups and repeated samples."
    )
    parser.add_argument("--config", type=pathlib.Path, help="JSON benchmark matrix")
    parser.add_argument("--rkvc", help="rkvc executable (overrides config)")
    parser.add_argument("--case", action="append", dest="case_names", help="run only this named config case; repeatable")
    parser.add_argument("--warmup", type=int, help="warmup runs per case")
    parser.add_argument("--iterations", type=int, help="measured runs per case")
    parser.add_argument("--timeout", type=float, help="timeout for one run in seconds")
    parser.add_argument("--cooldown", type=float, help="pause between runs in seconds")
    parser.add_argument("--results-dir", type=pathlib.Path, default=pathlib.Path(__file__).with_name("results"))
    parser.add_argument("--keep-work", action="store_true", help="keep generated media under results/work")
    parser.add_argument("--dry-run", action="store_true", help="validate and print commands without running")
    parser.add_argument("--quiet", action="store_true")

    direct = parser.add_argument_group("single case (used without --config)")
    direct.add_argument("--name")
    direct.add_argument("--operation", choices=sorted(OPERATIONS))
    direct.add_argument("--input")
    direct.add_argument("--output", help="temporary output filename; media is not retained by default")
    direct.add_argument("--codec", choices=sorted(CODECS), default="auto")
    direct.add_argument("--width", type=int)
    direct.add_argument("--height", type=int)
    direct.add_argument("--bitrate-bps", type=int)
    direct.add_argument("--qp", type=int)
    direct.add_argument("--frames", type=int, help="processed frame count, used to calculate FPS")
    direct.add_argument("--duration-seconds", type=float, help="clip duration, used to calculate realtime factor")
    return parser.parse_args(argv)


def _apply_overrides(config: RunnerConfig, args: argparse.Namespace) -> RunnerConfig:
    selected = config.cases
    if args.case_names:
        wanted = set(args.case_names)
        selected = tuple(case for case in selected if case.name in wanted)
        missing = wanted.difference(case.name for case in selected)
        if missing:
            raise BenchmarkError(f"unknown case name(s): {', '.join(sorted(missing))}")
    return dataclasses.replace(
        config,
        rkvc=args.rkvc or config.rkvc,
        warmup=config.warmup if args.warmup is None else args.warmup,
        iterations=config.iterations if args.iterations is None else args.iterations,
        timeout_seconds=config.timeout_seconds if args.timeout is None else args.timeout,
        cooldown_seconds=config.cooldown_seconds if args.cooldown is None else args.cooldown,
        cases=selected,
    )


def _validate_runner(config: RunnerConfig) -> None:
    if config.warmup < 0:
        raise BenchmarkError("warmup must be non-negative")
    if config.iterations <= 0:
        raise BenchmarkError("iterations must be positive")
    if config.timeout_seconds <= 0:
        raise BenchmarkError("timeout must be positive")
    if config.cooldown_seconds < 0:
        raise BenchmarkError("cooldown must be non-negative")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        config = load_config(args.config.resolve()) if args.config else _direct_config(args)
        config = _apply_overrides(config, args)
        _validate_runner(config)

        if args.dry_run:
            for case in config.cases:
                output = pathlib.Path("<work>") / case.output_name
                print(shlex.join(case.command(config.rkvc, output)))
            return 0

        args.results_dir = args.results_dir.resolve()
        work_root = args.results_dir / "work" if args.keep_work else None
        temporary: tempfile.TemporaryDirectory[str] | None = None
        if work_root is None:
            temporary = tempfile.TemporaryDirectory(prefix="rkvc-bench-")
            work_root = pathlib.Path(temporary.name)
        else:
            work_root.mkdir(parents=True, exist_ok=True)

        results: list[dict[str, Any]] = []
        try:
            for index, case in enumerate(config.cases):
                case_work = work_root / f"{index:02d}-{case.name}"
                case_work.mkdir(parents=True, exist_ok=True)
                results.append(
                    run_case(
                        config.rkvc,
                        case,
                        config.warmup,
                        config.iterations,
                        config.timeout_seconds,
                        config.cooldown_seconds,
                        case_work,
                        args.quiet,
                    )
                )
        finally:
            if temporary is not None:
                temporary.cleanup()

        report = {
            "schema_version": SCHEMA_VERSION,
            "system": system_metadata(),
            "rkvc": config.rkvc,
            "warmup": config.warmup,
            "iterations": config.iterations,
            "cases": results,
        }
        json_path, csv_path = write_artifacts(report, args.results_dir)
        if not args.quiet:
            _print_summary(report)
            print(f"\nJSON: {json_path}")
            print(f"CSV:  {csv_path}")
        return 1 if any(case["status"] != "passed" for case in results) else 0
    except BenchmarkError as exc:
        print(f"benchmark: error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("benchmark: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
