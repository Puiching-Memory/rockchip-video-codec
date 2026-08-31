"""Directory-scan discovery of dependency adapters.

Any ``*.py`` module under ``tools/rkvc_build/adapters/`` that defines a
subclass of ``DependencyAdapter`` is registered.  Discovery is deterministic
(module name order), never relying on ``os.listdir`` ordering.
"""

from __future__ import annotations

import importlib
from pathlib import Path
import pkgutil

from .base import DependencyAdapter

_PACKAGE = __name__


class AdapterRegistry:
    def __init__(self) -> None:
        self._classes: list[type[DependencyAdapter]] = []

    def scan(self) -> None:
        """Import adapter modules and collect ``DependencyAdapter`` subclasses."""
        modules = [
            m.name for m in pkgutil.iter_modules(__path__)
            if not m.name.startswith("_")
        ]
        for name in sorted(modules):
            module = importlib.import_module(f"{_PACKAGE}.{name}")
            for attr in vars(module).values():
                if (isinstance(attr, type) and issubclass(attr, DependencyAdapter)
                        and attr is not DependencyAdapter):
                    self._classes.append(attr)

    @property
    def classes(self) -> list[type[DependencyAdapter]]:
        return list(self._classes)

    def instantiate(self, work, logger) -> list[DependencyAdapter]:
        return [cls(work=work, logger=logger) for cls in self._classes]


def discover_adapters(work, logger) -> list[DependencyAdapter]:
    registry = AdapterRegistry()
    registry.scan()
    return registry.instantiate(work, logger)
