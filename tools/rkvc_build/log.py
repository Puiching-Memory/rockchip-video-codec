"""Minimal logging helper for the orchestrator."""

from __future__ import annotations

import sys
import time


class Logger:
    def __init__(self, name: str, verbose: bool = False, quiet: bool = False):
        self.name = name
        self.verbose = verbose
        self.quiet = quiet

    def _emit(self, level: str, message: str) -> None:
        if self.quiet and level not in ("error",):
            return
        if level == "debug" and not self.verbose:
            return
        flush = level in ("error", "warning")
        print(f"[{self.name}] [{level}] {message}", file=sys.stderr,
              flush=flush)

    def debug(self, message: str) -> None:
        self._emit("debug", message)

    def info(self, message: str) -> None:
        self._emit("info", message)

    def warning(self, message: str) -> None:
        self._emit("warning", message)

    def error(self, message: str) -> None:
        self._emit("error", message)

    def stage(self, name: str, detail: str = "") -> None:
        label = f"{name}" + (f" ({detail})" if detail else "")
        print(f"[{self.name}] ---- {label} ----", file=sys.stderr)
