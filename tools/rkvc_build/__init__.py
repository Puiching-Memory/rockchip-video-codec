"""rkvc release build orchestrator (Python stdlib only).

``tools/rkvc-build`` is the single release entry point.  It orchestrates
stages and passes explicit host/target/sysroot; each stage is cached by an
input digest.  Dependency adapters implement ``probe/fetch/build/install`` and
are discovered by directory scan.  It never links against the target library
and is never shipped in the ARM package.
"""

__version__ = "0.4.0"

from . import target, cache, log, stages
from .adapters import discover_adapters
from .verify import verify_package

__all__ = ["target", "cache", "log", "stages", "verify_package",
           "discover_adapters", "__version__"]
