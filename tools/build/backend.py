"""
PEP 517 build backend that wraps setuptools with aggressive caching.

This is a build backend entry point, NOT a library.
NO setup.py required - this backend injects extensions dynamically.

Caching strategy:
1. Cache built wheels (instant reinstall when nothing changed)
2. Cache metadata generation (avoid setuptools invocation)
3. Cache build requirements (avoid setuptools invocation)
4. Share state within single build process (prepare_metadata + build_wheel)
"""

from __future__ import annotations

import ctypes
import errno as errno_module
import hashlib
import json
import os
import shutil
import sys
import sysconfig
import time
from pathlib import Path
from typing import TYPE_CHECKING
from typing import Any

# Import setuptools backend to wrap
if TYPE_CHECKING:
    from collections.abc import Callable

    from setuptools import Extension


class SetuptoolsBackend:
    def __getattribute__(self, item: str) -> Any:
        from setuptools import build_meta

        return getattr(build_meta, item)


echo = print

setuptools_build_meta = SetuptoolsBackend()

# Project paths
PROJECT_ROOT = Path(__file__).parent.parent.parent
CACHE_ROOT = PROJECT_ROOT / ".build-cache"
CACHE_ROOT.mkdir(exist_ok=True)

WHEEL_CACHE = CACHE_ROOT / "wheels"
METADATA_CACHE = CACHE_ROOT / "metadata"
REQUIRES_CACHE = CACHE_ROOT / "requires"
WHEEL_CACHE.mkdir(exist_ok=True)
METADATA_CACHE.mkdir(exist_ok=True)
REQUIRES_CACHE.mkdir(exist_ok=True)

# Single-process state
_session_state: dict[str, Any] = {}

VECTOR_CALL_PATCH_AVAILABLE = sys.version_info >= (3, 12)

# ============================================================================
# Fast Copy (APFS clone on macOS)
# ============================================================================

if sys.platform == "darwin":
    _libc = ctypes.CDLL("libSystem.dylib", use_errno=True)
    _clonefile = getattr(_libc, "clonefile", None)
    _CLONE_NOOWNERCOPY = 0x0004
else:
    _clonefile = None


class NoExceptionError(Exception): ...


def _fast_copy(src: Path, dst: Path) -> None:
    """Copy using APFS clone on macOS, fallback to shutil.copy2."""
    if dst.exists():
        dst.unlink()

    if sys.platform == "darwin" and _clonefile is not None:
        _clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _clonefile.restype = ctypes.c_int
        ret = _clonefile(
            os.fsencode(str(src)),
            os.fsencode(str(dst)),
            ctypes.c_int(_CLONE_NOOWNERCOPY),
        )
        if ret == 0:
            return
        err = ctypes.get_errno()
        notsup = {
            getattr(errno_module, n, None)
            for n in ["EXDEV", "ENOTSUP", "EOPNOTSUPP", "ENOTTY"]
            if hasattr(errno_module, n)
        }
        if err not in notsup:
            raise OSError(err, "clonefile failed")
    shutil.copy2(src, dst)


def _fast_copy_tree(src: Path, dst: Path) -> None:
    """Copy directory tree, using fast copy for files."""
    if dst.exists():
        shutil.rmtree(dst)

    def copy_function(s: str, d: str) -> None:
        _fast_copy(Path(s), Path(d))

    shutil.copytree(src, dst, copy_function=copy_function)


# ============================================================================
# Fingerprinting
# ============================================================================


def _hash_file(path: Path) -> str:
    """Compute SHA256 hash of a file."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _hash_dict(d: dict[str, Any]) -> str:
    """Compute SHA256 hash of a dictionary."""
    return hashlib.sha256(json.dumps(d, sort_keys=True).encode()).hexdigest()


def _environment_fingerprint() -> dict[str, Any]:
    """Get fingerprint of build environment."""
    cfg = sysconfig.get_config_vars()
    return {
        "python": sys.version,
        "platform": sys.platform,
        "abi": cfg.get("ABIFLAGS"),
        "cc": cfg.get("CC"),
        "cflags": cfg.get("CFLAGS"),
        "soabi": cfg.get("SOABI"),
        "ext_suffix": cfg.get("EXT_SUFFIX"),
        "macosx_target": os.environ.get("MACOSX_DEPLOYMENT_TARGET"),
        "vector_call": VECTOR_CALL_PATCH_AVAILABLE,
    }


def _project_fingerprint() -> str:
    """Get fingerprint of project configuration."""
    pyproject = PROJECT_ROOT / "pyproject.toml"
    if pyproject.exists():
        return _hash_file(pyproject)
    return "no-pyproject"


# ============================================================================
# Extension Serialization
# ============================================================================


def _serialize_ext(ext: Extension) -> dict[str, Any]:
    """Serialize Extension object to dict."""
    return {
        "name": ext.name,
        "sources": ext.sources or [],
        "include_dirs": ext.include_dirs or [],
        "define_macros": ext.define_macros or [],
        "undef_macros": ext.undef_macros or [],
        "library_dirs": ext.library_dirs or [],
        "libraries": ext.libraries or [],
        "runtime_library_dirs": ext.runtime_library_dirs or [],
        "extra_objects": ext.extra_objects or [],
        "extra_compile_args": ext.extra_compile_args or [],
        "extra_link_args": ext.extra_link_args or [],
        "depends": ext.depends or [],
        "language": ext.language,
        "optional": getattr(ext, "optional", None),
    }


def _deserialize_ext(d: dict[str, Any]) -> Extension:
    """Deserialize dict to Extension object."""
    from setuptools import Extension

    ext = Extension(name=d["name"], sources=d["sources"])
    ext.include_dirs = d["include_dirs"]
    ext.define_macros = d["define_macros"]
    ext.undef_macros = d["undef_macros"]
    ext.library_dirs = d["library_dirs"]
    ext.libraries = d["libraries"]
    ext.runtime_library_dirs = d["runtime_library_dirs"]
    ext.extra_objects = d["extra_objects"]
    ext.extra_compile_args = d["extra_compile_args"]
    ext.extra_link_args = d["extra_link_args"]
    ext.depends = d["depends"]
    if d.get("language"):
        ext.language = d["language"]
    if d.get("optional") is not None:
        ext.optional = d["optional"]
    return ext


# ============================================================================
# C Extensions
# ============================================================================


def _get_c_extensions() -> list[Extension]:
    """Get C extensions."""
    from setuptools import Extension

    python_include = Path(sysconfig.get_paths()["include"])
    return [
        Extension(
            "copyc",
            sources=["src/copy.c", "src/_copying.c", "src/_pinning.c", "src/_patching.c"],
            include_dirs=[str(python_include), str(python_include / "internal")],
        )
    ]


# ============================================================================
# Extension Injection
# ============================================================================


def _inject_extensions() -> Callable[..., Any]:
    """Monkey-patch setuptools.setup() to inject extensions."""
    import setuptools

    original_setup = setuptools.setup
    extensions = _get_c_extensions()

    def patched_setup(*args: Any, **kwargs: Any) -> Any:
        if extensions:
            kwargs.setdefault("ext_modules", []).extend(extensions)
        return original_setup(*args, **kwargs)

    setuptools.setup = patched_setup
    return original_setup


def _restore_setup(original: Callable[..., Any]) -> None:
    """Restore original setuptools.setup()."""
    import setuptools

    setuptools.setup = original


# ============================================================================
# Wheel Fingerprinting
# ============================================================================


def _wheel_fingerprint(build_type: str) -> str:
    """Compute fingerprint for wheel caching."""
    sources_hash = hashlib.sha256()

    # 1) Hash Python sources from the project (assume src-layout)
    src_root = PROJECT_ROOT / "src"
    if src_root.exists():
        for py in sorted(src_root.rglob("*.py")):
            sources_hash.update(py.read_bytes())

    # 2) Hash C/C++ sources and headers that the extensions actually use
    #    (use the file lists declared by setuptools.Extension)
    exts = _get_c_extensions()
    c_like_exts = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}

    def _abs(p: str) -> Path:
        pth = Path(p)
        return pth if pth.is_absolute() else (PROJECT_ROOT / pth)

    seen: set[Path] = set()

    for e in exts:
        # declared source files
        for s in e.sources or []:
            p = _abs(s)
            if p.suffix.lower() in c_like_exts and p.exists() and p not in seen:
                sources_hash.update(p.read_bytes())
                seen.add(p)

        # declared dependencies (headers, generated files, etc.)
        for d in getattr(e, "depends", []) or []:
            p = _abs(d)
            if p.suffix.lower() in c_like_exts and p.exists() and p not in seen:
                sources_hash.update(p.read_bytes())
                seen.add(p)

        # heuristic: also hash nearby headers next to each source
        for s in e.sources or []:
            sp = _abs(s)
            if sp.exists():
                for h in sp.parent.glob("*.h"):
                    if h not in seen:
                        sources_hash.update(h.read_bytes())
                        seen.add(h)

    # 3) Hash pyproject.toml if present
    pyproject = PROJECT_ROOT / "pyproject.toml"
    if pyproject.exists():
        sources_hash.update(pyproject.read_bytes())

    fp = {
        "type": build_type,
        "sources": sources_hash.hexdigest(),
        "env": _environment_fingerprint(),
        # Keep this so changes to the *configuration* (names, args) also bust cache
        "extensions": [_serialize_ext(e) for e in exts],
        "project_fp": _project_fingerprint(),
    }
    return _hash_dict(fp)


# ============================================================================
# PEP 517 Backend Hooks
# ============================================================================


def get_requires_for_build_wheel(config_settings: dict[str, Any] | None = None) -> list[str]:
    """Get build requirements for wheel."""
    cache_key = f"wheel-{_project_fingerprint()}"
    cache_file = REQUIRES_CACHE / f"{cache_key}.json"

    # Check cache
    if cache_file.exists():
        try:
            cached = json.loads(cache_file.read_text())
            echo("[build-cache] Using cached build requirements")
            return cached["requires"]
        except NoExceptionError as e:
            echo(f"[build-cache] Requires cache load failed: {e}")
            cache_file.unlink(missing_ok=True)

    # Generate
    echo("[build-cache] Getting build requirements...")
    result = setuptools_build_meta.get_requires_for_build_wheel(config_settings)

    # Cache
    try:
        cache_file.write_text(json.dumps({"requires": result, "timestamp": time.time()}, indent=2))
        echo("[build-cache] Cached build requirements")
    except NoExceptionError as e:
        echo(f"[build-cache] Requires cache save failed: {e}")

    return result


def get_requires_for_build_editable(config_settings: dict[str, Any] | None = None) -> list[str]:
    """Get build requirements for editable install."""
    cache_key = f"editable-{_project_fingerprint()}"
    cache_file = REQUIRES_CACHE / f"{cache_key}.json"

    # Check cache
    if cache_file.exists():
        try:
            cached = json.loads(cache_file.read_text())
            echo("[build-cache] Using cached build requirements")
            return cached["requires"]
        except NoExceptionError as e:
            echo(f"[build-cache] Requires cache load failed: {e}")
            cache_file.unlink(missing_ok=True)

    # Generate
    echo("[build-cache] Getting build requirements...")
    result = setuptools_build_meta.get_requires_for_build_editable(config_settings)

    # Cache
    try:
        cache_file.write_text(json.dumps({"requires": result, "timestamp": time.time()}, indent=2))
        echo("[build-cache] Cached build requirements")
    except NoExceptionError as e:
        echo(f"[build-cache] Requires cache save failed: {e}")

    return result


def prepare_metadata_for_build_wheel(
    metadata_directory: str, config_settings: dict[str, Any] | None = None
) -> str:
    """Prepare metadata for wheel build."""
    # Check session state
    if "wheel_metadata" in _session_state:
        cached_info = _session_state["wheel_metadata"]
        echo("[build-cache] Reusing metadata from session")
        src = Path(cached_info["path"])
        dist_info_name = cached_info["name"]
        dst = Path(metadata_directory) / dist_info_name
        _fast_copy_tree(src, dst)
        return dist_info_name

    # Check cross-session cache
    fingerprint = _wheel_fingerprint("wheel")
    cache_dir = METADATA_CACHE / f"wheel-{fingerprint}"
    info_file = cache_dir / ".dist-info-name"

    if cache_dir.exists() and info_file.exists():
        try:
            dist_info_name = info_file.read_text().strip()
            echo("[build-cache] Using cached metadata")
            dst = Path(metadata_directory) / dist_info_name
            _fast_copy_tree(cache_dir / dist_info_name, dst)
            _session_state["wheel_metadata"] = {
                "path": str(dst),
                "name": dist_info_name,
            }
        except NoExceptionError as e:
            echo(f"[build-cache] Metadata cache load failed: {e}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return dist_info_name

    # Generate
    echo("[build-cache] Generating metadata...")
    original = _inject_extensions()
    try:
        result = setuptools_build_meta.prepare_metadata_for_build_wheel(
            metadata_directory, config_settings
        )
    finally:
        _restore_setup(original)

    # Cache
    src = Path(metadata_directory) / result
    try:
        cache_dir.mkdir(exist_ok=True)
        info_file.write_text(result)
        _fast_copy_tree(src, cache_dir / result)
        echo("[build-cache] Cached metadata")
    except NoExceptionError as e:
        echo(f"[build-cache] Metadata cache save failed: {e}")

    _session_state["wheel_metadata"] = {"path": str(src), "name": result}
    return result


def prepare_metadata_for_build_editable(
    metadata_directory: str, config_settings: dict[str, Any] | None = None
) -> str:
    """Prepare metadata for editable install."""
    # Check session state
    if "editable_metadata" in _session_state:
        cached_info = _session_state["editable_metadata"]
        echo("[build-cache] Reusing metadata from session")
        src = Path(cached_info["path"])
        dist_info_name = cached_info["name"]
        dst = Path(metadata_directory) / dist_info_name
        _fast_copy_tree(src, dst)
        return dist_info_name

    # Check cross-session cache
    fingerprint = _wheel_fingerprint("editable")
    cache_dir = METADATA_CACHE / f"editable-{fingerprint}"
    info_file = cache_dir / ".dist-info-name"

    if cache_dir.exists() and info_file.exists():
        try:
            dist_info_name = info_file.read_text().strip()
            echo("[build-cache] Using cached metadata")
            dst = Path(metadata_directory) / dist_info_name
            _fast_copy_tree(cache_dir / dist_info_name, dst)
            _session_state["editable_metadata"] = {
                "path": str(dst),
                "name": dist_info_name,
            }
        except NoExceptionError as e:
            echo(f"[build-cache] Metadata cache load failed: {e}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return dist_info_name

    # Generate
    echo("[build-cache] Generating metadata...")
    original = _inject_extensions()
    try:
        result = setuptools_build_meta.prepare_metadata_for_build_editable(
            metadata_directory, config_settings
        )
    finally:
        _restore_setup(original)

    # Cache
    src = Path(metadata_directory) / result
    try:
        cache_dir.mkdir(exist_ok=True)
        info_file.write_text(result)
        _fast_copy_tree(src, cache_dir / result)
        echo("[build-cache] Cached metadata")
    except NoExceptionError as e:
        echo(f"[build-cache] Metadata cache save failed: {e}")

    _session_state["editable_metadata"] = {"path": str(src), "name": result}
    return result


def build_wheel(
    wheel_directory: str,
    config_settings: dict[str, Any] | None = None,
    metadata_directory: str | None = None,
) -> str:
    """Build a wheel."""
    if os.environ.get("DUPER_DISABLE_WHEEL_CACHE") == "1":
        echo("[build-cache] Wheel caching disabled")
        original = _inject_extensions()
        try:
            return setuptools_build_meta.build_wheel(
                wheel_directory, config_settings, metadata_directory
            )
        finally:
            _restore_setup(original)

    # Check cache
    fingerprint = _wheel_fingerprint("wheel")
    cache_dir = WHEEL_CACHE / f"wheel-{fingerprint}"
    cache_file = cache_dir / "wheel.whl"
    name_file = cache_dir / ".wheel-name"

    if cache_file.exists() and name_file.exists():
        try:
            wheel_name = name_file.read_text().strip()
            echo("[build-cache] Using cached wheel (instant)")
            dst = Path(wheel_directory) / wheel_name
            _fast_copy(cache_file, dst)
        except NoExceptionError as e:
            echo(f"[build-cache] Cache load failed: {e}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return wheel_name

    # Build
    echo("[build-cache] Building wheel...")
    original = _inject_extensions()
    try:
        result = setuptools_build_meta.build_wheel(
            wheel_directory, config_settings, metadata_directory
        )
    finally:
        _restore_setup(original)

    # Cache
    src = Path(wheel_directory) / result
    try:
        cache_dir.mkdir(exist_ok=True)
        name_file.write_text(result)
        _fast_copy(src, cache_file)
        echo("[build-cache] Cached wheel")
    except NoExceptionError as e:
        echo(f"[build-cache] Cache save failed: {e}")

    return result


def build_editable(
    wheel_directory: str,
    config_settings: dict[str, Any] | None = None,
    metadata_directory: str | None = None,
) -> str:
    """Build an editable wheel."""
    if os.environ.get("DUPER_DISABLE_WHEEL_CACHE") == "1":
        echo("[build-cache] Wheel caching disabled")
        original = _inject_extensions()
        try:
            return setuptools_build_meta.build_editable(
                wheel_directory, config_settings, metadata_directory
            )
        finally:
            _restore_setup(original)

    # Check cache
    fingerprint = _wheel_fingerprint("editable")
    cache_dir = WHEEL_CACHE / f"editable-{fingerprint}"
    cache_file = cache_dir / "wheel.whl"
    name_file = cache_dir / ".wheel-name"

    if cache_file.exists() and name_file.exists():
        try:
            wheel_name = name_file.read_text().strip()
            echo("[build-cache] Using cached editable wheel (instant)")
            dst = Path(wheel_directory) / wheel_name
            _fast_copy(cache_file, dst)
        except NoExceptionError as e:
            echo(f"[build-cache] Cache load failed: {e}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return wheel_name

    # Build
    echo("[build-cache] Building editable wheel...")
    original = _inject_extensions()
    try:
        result = setuptools_build_meta.build_editable(
            wheel_directory, config_settings, metadata_directory
        )
    finally:
        _restore_setup(original)

    # Cache
    src = Path(wheel_directory) / result
    try:
        cache_dir.mkdir(exist_ok=True)
        name_file.write_text(result)
        _fast_copy(src, cache_file)
        echo("[build-cache] Cached editable wheel")
    except NoExceptionError as e:
        echo(f"[build-cache] Cache save failed: {e}")

    return result


def __getattr__(name: str) -> Any:
    from setuptools import build_meta

    return getattr(build_meta, name)
