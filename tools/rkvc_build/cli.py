"""Command-line entry for ``tools/rkvc-build``.

Orchestrates the single release pipeline:

    resolve -> host tools -> target deps -> build target -> install to staging
            -> seal -> verify -> archive

All steps are Python-stdlib.  This tool never links against the target library
and is never shipped in the ARM package.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from . import __version__
from .log import Logger
from .stages import Pipeline, Stage, StageError
from .target import BuildContext
from .verify import verify_package


def _cmd_package(args: argparse.Namespace) -> int:
    logger = Logger("rkvc-build", verbose=args.verbose, quiet=args.quiet)
    if bool(args.rknn_sdk) != bool(args.rknn_license):
        logger.error("--rknn-sdk and --rknn-license must be provided together")
        return 2
    base = Path(args.workdir).resolve()
    ctx = BuildContext.create(args.target, base, jobs=args.jobs)
    # RKNN Runtime is never inferred from a stale target prefix: each package
    # invocation must explicitly authorize its audited SDK input.
    ctx.rknn_enabled = bool(args.rknn_sdk)
    ctx.staging.mkdir(parents=True, exist_ok=True)
    dist = base / "dist"
    state = {"pkg_root": None, "version": None, "archive": None}

    def _sysroot(c):
        from .sysroot import ensure_sysroot
        dest = c.work / "sysroots" / "ubuntu-20.04-arm64"
        c.target.sysroot = ensure_sysroot(dest, refresh=args.refresh_sysroot)

    def _deps_sodium(c):
        from .adapters.sodium import SodiumAdapter
        adapter = SodiumAdapter(c.work, logger)
        dep = adapter.probe(c.target)
        if dep is None:
            logger.info("deps-sodium: not applicable; skipped")
            return
        result = adapter.fetch(dep)
        if not result.ok:
            raise StageError(f"deps-sodium fetch failed: {result.message}")
        result = adapter.build(dep, c.host_prefix, c.target_prefix)
        if not result.ok:
            raise StageError(f"deps-sodium build failed: {result.message}")

    def _deps_mpp(c):
        import os
        from .adapters.mpp import MppAdapter
        adapter = MppAdapter(c.work, logger)
        dep = adapter.probe(c.target)
        if dep is None:
            logger.info("deps-mpp: not applicable; skipped")
            return
        os.environ["RKVC_SYSROOT"] = str(c.target.sysroot)
        result = adapter.fetch(dep)
        if not result.ok:
            raise StageError(f"deps-mpp fetch failed: {result.message}")
        result = adapter.build(dep, c.host_prefix, c.target_prefix)
        if not result.ok:
            raise StageError(f"deps-mpp build failed: {result.message}")

    def _deps_rga(c):
        from .adapters.rga import RgaAdapter
        adapter = RgaAdapter(c.work, logger)
        dep = adapter.probe(c.target)
        if dep is None:
            logger.info("deps-rga: not applicable; skipped")
            return
        result = adapter.fetch(dep)
        if not result.ok:
            raise StageError(f"deps-rga fetch failed: {result.message}")
        result = adapter.build(dep, c.host_prefix, c.target_prefix)
        if not result.ok:
            raise StageError(f"deps-rga build failed: {result.message}")

    def _deps_rknn(c):
        from .adapters.rknn import RknnAdapter
        adapter = RknnAdapter(c.work, logger, args.rknn_sdk,
                              args.rknn_license)
        dep = adapter.probe(c.target)
        if dep is None:
            raise StageError(
                "deps-rknn: SDK must contain include/rknn_api.h and "
                "lib/librknnrt.so, and --rknn-license must name a file")
        result = adapter.fetch(dep)
        if not result.ok:
            raise StageError(f"deps-rknn fetch failed: {result.message}")
        result = adapter.build(dep, c.host_prefix, c.target_prefix)
        if not result.ok:
            raise StageError(f"deps-rknn stage failed: {result.message}")

    def _build_install(c):
        from . import cmake_stage
        state["pkg_root"] = cmake_stage.build_and_install(c, logger)
        state["version"] = cmake_stage.package_version()

    def _release_meta(c):
        from .sbom import aggregate_legal, write_sbom
        write_sbom(state["pkg_root"], state["version"], c.target.name)
        aggregate_legal(state["pkg_root"])

    def _seal(c):
        from .seal import seal
        seal(state["pkg_root"], c.target.name, state["version"])

    def _verify(c):
        report = verify_package(
            state["pkg_root"], c.target.name,
            sysroot=c.target.sysroot,
            max_glibc=c.target.libc_floor)
        print(report.format())
        if not report.ok:
            raise StageError("package verification failed")

    def _archive(c):
        from .archive import create_deterministic_tar_gz
        out = dist / f"{state['pkg_root'].name}.tar.gz"
        create_deterministic_tar_gz(state["pkg_root"], out,
                                    state["pkg_root"].name)
        state["archive"] = out
        logger.info(f"archive: {out}")

    def _smoke(c):
        from .smoke import qemu_smoke
        qemu_smoke(state["pkg_root"], c.target.sysroot, logger)

    # 阶段只声明输入身份；构建增量由 cmake/ninja 自身承担，
    # sysroot 幂等由其 stamp 文件承担，归档每次重算以暴露非确定性。
    pipeline = Pipeline(ctx, logger=logger)
    pipeline.add(Stage("sysroot", _sysroot, always_run=True,
                       inputs={"target": ctx.target.name}))
    pipeline.add(Stage("deps-sodium", _deps_sodium, always_run=True,
                       inputs={"target": ctx.target.name}))
    if not args.no_mpp:
        pipeline.add(Stage("deps-mpp", _deps_mpp, always_run=True,
                           inputs={"target": ctx.target.name}))
    if not args.no_rga:
        pipeline.add(Stage("deps-rga", _deps_rga, always_run=True,
                           inputs={"target": ctx.target.name}))
    if args.rknn_sdk:
        pipeline.add(Stage("deps-rknn", _deps_rknn, always_run=True,
                           inputs={"target": ctx.target.name,
                                   "sdk": str(args.rknn_sdk.resolve()),
                                   "license": str(
                                       args.rknn_license.resolve())}))
    pipeline.add(Stage("build-install", _build_install, always_run=True,
                       inputs={"target": ctx.target.name}))
    # SBOM/许可证须先于 seal，以纳入 SHA256SUMS 覆盖
    pipeline.add(Stage("release-meta", _release_meta, always_run=True,
                       inputs={"target": ctx.target.name}))
    pipeline.add(Stage("seal", _seal, always_run=True,
                       inputs={"target": ctx.target.name}))
    pipeline.add(Stage("verify", _verify, always_run=True,
                       inputs={"target": ctx.target.name}))
    pipeline.add(Stage("archive", _archive, always_run=True,
                       inputs={"target": ctx.target.name}))
    if not args.no_smoke:
        pipeline.add(Stage("qemu-smoke", _smoke, always_run=True,
                           inputs={"target": ctx.target.name}))

    try:
        pipeline.run_all()
    except Exception as exc:  # noqa: BLE001
        logger.error(str(exc))
        return 1
    logger.info(f"package ok: {state['archive']}")
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    logger = Logger("rkvc-build", verbose=args.verbose, quiet=args.quiet)
    package = Path(args.package).resolve()
    floor = tuple(int(x) for x in args.glibc_floor.split("."))
    report = verify_package(
        package, args.target,
        sysroot=Path(args.sysroot).resolve() if args.sysroot else None,
        max_glibc=floor)
    print(report.format())
    if not report.ok:
        logger.error(f"package verification failed for {package}")
        return 1
    return 0


def _cmd_inspect(args: argparse.Namespace) -> int:
    from .verify.elf import read_elf
    info = read_elf(str(Path(args.file).resolve()))
    print(f"machine:   {info.machine}")
    print(f"interp:    {info.interpreter}")
    print(f"soname:    {info.dynamic.soname}")
    print(f"needed:    {', '.join(info.dynamic.needed)}")
    print(f"max glibc: {info.max_glibc}")
    print(f"versions:  {', '.join(info.all_versions)}")
    print(f"load W+X:  {info.load_wx}   exec-stack: {info.stack_exec}")
    return 0

def _cmd_sysroot(args: argparse.Namespace) -> int:
    from .sysroot import ensure_sysroot
    try:
        ensure_sysroot(args.dest.resolve(), refresh=args.refresh)
    except Exception as exc:  # noqa: BLE001
        Logger("rkvc-build").error(str(exc))
        return 1
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="rkvc-build",
        description="rkvc release build orchestrator (stdlib only).")
    parser.add_argument("--version", action="version",
                        version=f"rkvc-build {__version__}")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("-q", "--quiet", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    pkg = sub.add_parser("package", help="build the release package")
    pkg.add_argument("--target", default="linux-aarch64-glibc231",
                     help="target ABI string (e.g. linux-aarch64-glibc231)")
    pkg.add_argument("--workdir", default=".build/portable",
                     help="base work directory (default .build/portable)")
    pkg.add_argument("--jobs", type=int, default=1)
    pkg.add_argument("--refresh-sysroot", action="store_true",
                     help="re-resolve sysroot packages instead of using the lockfile")
    pkg.add_argument("--no-smoke", action="store_true",
                     help="skip the qemu smoke stage")
    pkg.add_argument("--no-mpp", action="store_true",
                     help="skip the Rockchip MPP dependency and backend DSO")
    pkg.add_argument("--no-rga", action="store_true",
                     help="skip the Rockchip RGA dependency and backend DSO")
    pkg.add_argument("--rknn-sdk", type=Path,
                     help="audited RKNN Runtime SDK prefix (include/ + lib/)")
    pkg.add_argument("--rknn-license", type=Path,
                     help="license/redistribution evidence for --rknn-sdk")
    pkg.add_argument("-m", "--model-target", action="append", default=[],
                     help="model compilation target (may be repeated)")
    pkg.add_argument("--for-device", type=Path,
                     help="probe.json to deterministically trim the staging tree")
    pkg.set_defaults(func=_cmd_package)

    ver = sub.add_parser("verify", help="verify a staging tree / package")
    ver.add_argument("package", type=Path)
    ver.add_argument("--target", default="linux-aarch64-glibc231")
    ver.add_argument("--sysroot", type=Path)
    ver.add_argument("--glibc-floor", default="2.31")
    ver.set_defaults(func=_cmd_verify)

    ins = sub.add_parser("inspect", help="inspect a single ELF file")
    ins.add_argument("file", type=Path)
    ins.set_defaults(func=_cmd_inspect)

    sysr = sub.add_parser("sysroot", help="fetch the pinned target sysroot")
    sysr.add_argument("--dest", type=Path,
                      default=Path(".build/sysroots/ubuntu-20.04-arm64"))
    sysr.add_argument("--refresh", action="store_true",
                      help="re-resolve package versions (rewrites the lockfile)")
    sysr.set_defaults(func=_cmd_sysroot)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
