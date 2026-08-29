"""Loads the generated constants module build/gen/omgp_protocol.py by path.

Every reference-implementation module imports protocol values from here so that the
YAML stays the single source of truth (constitution Principle I). Nothing in
tools/refimpl/ may spell an opcode, error code, TLV type or limit as a literal.
"""
from __future__ import annotations

import importlib.util
import pathlib
import types

ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN_DIR = ROOT / "build" / "gen"


def load(gen_dir: pathlib.Path = GEN_DIR) -> types.ModuleType:
    path = gen_dir / "omgp_protocol.py"
    if not path.exists():
        raise FileNotFoundError(
            f"{path} missing — run `python3 tools/codegen.py` (or ./pipeline.sh codegen) first")
    spec = importlib.util.spec_from_file_location(f"omgp_protocol_{abs(hash(str(path)))}", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


_cached: types.ModuleType | None = None


def P() -> types.ModuleType:
    """The generated module for the repository's build/gen (cached)."""
    global _cached
    if _cached is None:
        _cached = load()
    return _cached
