"""Stage orchestration.

A build is a sequence of named stages.  Each stage runs only if its inputs
changed: a deterministic stage key is derived from the declared inputs plus the
host/target identity.  The orchestrator never copies a hard-coded list of
"should exist" binaries -- the CMake install tree is the sole source of the
staging content.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from . import cache as cache_mod
from .log import Logger
from .target import BuildContext


class StageError(RuntimeError):
    pass


@dataclass
class Stage:
    name: str
    run: callable
    inputs: dict = field(default_factory=dict)
    outputs: list[Path] = field(default_factory=list)
    always_run: bool = False


class Pipeline:
    def __init__(self, context: BuildContext,
                 cache: cache_mod.Cache | None = None,
                 logger: Logger | None = None):
        self.ctx = context
        self.cache = cache if cache is not None else cache_mod.Cache(
            context.cache)
        self.log = logger if logger is not None else Logger("rkvc-build")
        self.stages: list[Stage] = []

    def add(self, stage: Stage) -> None:
        self.stages.append(stage)

    def key_for(self, stage: Stage) -> str:
        payload = {
            "stage": stage.name,
            "host": {
                "machine": self.ctx.host.machine,
                "system": self.ctx.host.system,
            },
            "target": {
                "name": self.ctx.target.name,
                "arch": self.ctx.target.arch,
                "libc_floor": list(self.ctx.target.libc_floor),
            },
            "inputs": stage.inputs,
        }
        return cache_mod.Cache.digest(payload)

    def run_all(self) -> None:
        for stage in self.stages:
            key = self.key_for(stage)
            # Restore cached outputs if present.
            if not stage.always_run and self.cache.has(key):
                self.log.info(
                    f"stage {stage.name}: cache hit ({key[:12]}...)")
                for path in stage.outputs:
                    path.parent.mkdir(parents=True, exist_ok=True)
                self.cache.restore(key, path.parent if len(stage.outputs) == 1
                                   else self.ctx.staging)
                continue

            self.log.stage(stage.name)
            try:
                stage.run(self.ctx)
            except Exception as exc:  # noqa: BLE001
                self.log.error(f"stage {stage.name} failed: {exc}")
                raise StageError(f"stage {stage.name} failed: {exc}") from exc

            if stage.outputs and not stage.always_run:
                self.cache.save(key, stage.outputs)
                self.log.debug(f"stage {stage.name}: cached ({key[:12]}...)")
