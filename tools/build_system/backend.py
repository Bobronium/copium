"""
PEP 517 build backend that wraps setuptools with aggressive caching + setuptools-scm.

This is a build backend entry point, NOT a library.
NO setup.py required - this backend injects extensions dynamically.

Caching strategy:
1. Cache built wheels (instant reinstall when nothing changed)
2. Cache metadata generation (avoid setuptools invocation)
3. Cache build requirements (avoid setuptools invocation)
4. Share state within single build process (prepare_metadata + build_wheel)

Version strategy (via setuptools-scm + optional build hash):

- Tags (v1.2.3)     -> release version: 1.2.3
- Main commits      -> dev version: 1.2.4.dev42
- Dirty working dir -> dev version: 1.2.4.dev42
- Local dev builds (COPIUM_LOCAL_DEVELOPMENT set):

    <scm public>+<build_hash>

  where:
    <scm public> is setuptools-scm's public version, e.g. "0.1.0a1.dev57"
    <build_hash> is a fingerprint over sources + build config.

Build hash / fingerprint strategy:

- Depends on:
    * All src/** files with extensions: py, pyi, c, cc, cpp, cxx, h, hh, hpp, hxx
    * pyproject.toml
    * This backend file
    * _copium_autopatch.pth (if present)
    * Selected environment details and build command (config_settings)
- Used as:
    * Cache key for wheels and metadata
    * Local version segment under COPIUM_LOCAL_DEVELOPMENT
    * C macro COPIUM_BUILD_HASH
"""

from __future__ import annotations

import ctypes
import errno as errno_module
import hashlib
import json
import os
import shlex
import shutil
import sys
import sysconfig
import time
from pathlib import Path
from typing import TYPE_CHECKING
from typing import Any

from packaging.version import Version

# Import setuptools backend to wrap
if TYPE_CHECKING:
    from collections.abc import Callable
    from setuptools import Extension


class SetuptoolsBackend:
    def __getattribute__(self, item: str) -> Any:
        from setuptools import build_meta

        return getattr(build_meta, item)


def echo(*args: Any, **kwargs: Any) -> None:
    print("[copium-build-system]", *args, **kwargs)  # noqa: T201


def error(*args: Any, **kwargs: Any) -> None:
    echo(*args, **kwargs, file=sys.stderr)


setuptools_build_meta = SetuptoolsBackend()

# Project paths
PROJECT_ROOT = Path(__file__).parent.parent.parent
CACHE_ROOT = PROJECT_ROOT / ".build-cache"
CACHE_ROOT.mkdir(exist_ok=True)

WHEEL_CACHE = CACHE_ROOT / "wheels"
METADATA_CACHE = CACHE_ROOT / "metadata"
REQUIRES_CACHE = CACHE_ROOT / "requires"
for d in (WHEEL_CACHE, METADATA_CACHE, REQUIRES_CACHE):
    d.mkdir(exist_ok=True)

# Single-process state
_session_state: dict[str, Any] = {}

BACKEND_PATH = Path(__file__)
SOURCE_SUFFIXES = {
    ".py",
    ".pyi",
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
}

# ============================================================================
# Version Management (setuptools-scm integration)
# ============================================================================


def _get_version_tuple(version: str) -> tuple[int, int, int, str | None, int | None, str]:
    """
    Convert a PEP 440 version string into the internal VersionInfo tuple shape:

        (major, minor, patch, pre, dev, local)

    where:
      * pre   -> 'a1', 'b1', 'rc2', or None
      * dev   -> integer dev number from `.devN`, or None
      * local -> build hash (preferred) or normalized local segment / sentinel
    """
    v = Version(version)

    pre = None
    if v.pre is not None:
        label, num = v.pre  # ('a'|'b'|'rc', int)
        pre = f"{label}{int(num)}"

    return v.major, v.minor, v.micro, pre, v.dev, v.local


def _get_version_info(build_hash: str | None = None) -> dict[str, Any]:
    """Get version and parsed tuple from setuptools-scm.

    Behavior:

    - Always ask setuptools-scm for the "authoritative" version.
      Example: 0.1.0a1.dev57
    - Parse it with packaging.Version.
    - If COPIUM_LOCAL_DEVELOPMENT is set AND build_hash is provided:
        version = f"{v.public}+{build_hash}"
      where v.public == "0.1.0a1.dev57".
    - Otherwise, never include a local version segment (no "+" part).
    """
    fallback = {
        "version": "0.0.0+unknown",
        "commit_id": None,
        "version_tuple": (0, 0, 0, None, None, "unknown"),
    }

    local_mode = bool(os.environ.get("COPIUM_LOCAL_DEVELOPMENT"))

    try:
        from setuptools_scm import get_version

        base_version = get_version(
            root=str(PROJECT_ROOT),
            version_scheme="guess-next-dev",
            local_scheme="no-local-version",
        )
        echo("setuptools-scm:", base_version)
    except NoExceptionError as e:
        error(f"Version detection failed via setuptools-scm: {e!r}")
        return fallback

    try:
        v = Version(base_version)
    except NoExceptionError as e:
        error(f"Base version parsing failed for {base_version!r}: {e!r}")
        return fallback

    if local_mode and build_hash:
        # In local development builds, append our own build hash as the local segment.
        version = f"{v.public}+{build_hash}"
        tuple_build_hash = build_hash
        echo(
            "COPIUM_LOCAL_DEVELOPMENT is set; overriding local version "
            f"part with build hash: {version}"
        )
    else:
        # For CI / release builds, never ship a local segment so PyPI accepts the version.
        version = v.public

    version_tuple = _get_version_tuple(version)

    echo(f"Version: {version}")
    echo(f"Tuple: {version_tuple}")

    # Commit ID is embedded by setuptools-scm in base_version (gXXXX...), if present.
    # For now we don't try to parse it; leave as None.
    commit_id = None

    return {
        "version": version,
        "commit_id": commit_id,
        "version_tuple": version_tuple,
    }


def _version_info_to_macros(version_info: dict[str, Any]) -> list[tuple[str, str]]:
    """Convert version info to C preprocessor macros."""
    major, minor, patch, pre, dev, local = version_info["version_tuple"]

    macros = [
        ("COPIUM_VERSION", f'"{version_info["version"]}"'),
        ("COPIUM_VERSION_MAJOR", str(major)),
        ("COPIUM_VERSION_MINOR", str(minor)),
        ("COPIUM_VERSION_PATCH", str(patch)),
    ]
    if version_info["commit_id"]:
        macros.append(("COPIUM_COMMIT_ID", f'"{version_info["commit_id"]}"'))

    # Pre-release: 'a1', 'b1', 'rc2', etc.
    if pre is not None:
        if '"' in str(pre):
            error(f"malformed {pre=}, skipping")
        else:
            macros.append(("COPIUM_VERSION_PRE", f'"{pre}"'))

    # Dev number: integer, if present.
    if dev is not None:
        if not isinstance(dev, int):
            error(f"malformed {dev=}, expected valid int, skipping")
            # still skip, loudly
        else:
            macros.append(("COPIUM_VERSION_DEV", str(dev)))

    # The local segment (build hash) is passed separately as COPIUM_BUILD_HASH.

    return macros


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
    """Compute SHA256 hash of a dictionary (shortened)."""
    return hashlib.sha256(json.dumps(d, sort_keys=True).encode()).hexdigest()[:8]


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
    }


def _project_fingerprint() -> str:
    """Get fingerprint of project configuration (pyproject.toml)."""
    pyproject = PROJECT_ROOT / "pyproject.toml"
    if pyproject.exists():
        return _hash_file(pyproject)
    return "no-pyproject"


def _build_command_fingerprint(
    config_settings: dict[str, Any] | None,
) -> dict[str, Any]:
    """Normalize build command/config + relevant env overrides for hashing."""
    cfg = config_settings or {}

    normalized_cfg: dict[str, Any] = {}
    for key, value in sorted(cfg.items(), key=lambda kv: kv[0]):
        if isinstance(value, (list, tuple)):
            normalized_cfg[key] = list(value)
        else:
            normalized_cfg[key] = value

    # Capture env vars that realistically affect compilation
    interesting_env: dict[str, str] = {}
    for k in sorted(os.environ):
        if k.startswith(("COPIUM_", "CFLAGS", "CPPFLAGS", "LDFLAGS")):
            interesting_env[k] = os.environ[k]

    return {
        "config_settings": normalized_cfg,
        "env_overrides": interesting_env,
    }


def _sources_fingerprint() -> str:
    """Hash all relevant source files to detect code changes.

    Extremely literal:
    - Walks PROJECT_ROOT / "src"
    - Considers only suffixes in _SOURCE_SUFFIXES
    - Hashes (relative path, contents) of each file
    - Also folds in pyproject.toml, backend, and _copium_autopatch.pth
    """
    h = hashlib.sha256()
    count = 0

    src_root = PROJECT_ROOT / "src"
    if not src_root.exists():
        error(f"Source root {src_root} does not exist; using 'no-src' fingerprint")
        return "no-src"

    for p in sorted(src_root.rglob("*")):
        if p.is_file() and p.suffix.lower() in SOURCE_SUFFIXES:
            try:
                rel = p.relative_to(PROJECT_ROOT).as_posix().encode()
            except ValueError:
                rel = p.as_posix().encode()
            try:
                h.update(rel)
                h.update(p.read_bytes())
                count += 1
            except OSError as e:
                error(f"Failed to read source file for fingerprint {p}: {e!r}")
                h.update(f"ERROR:{p}".encode())

    # pyproject.toml
    pyproject = PROJECT_ROOT / "pyproject.toml"
    if pyproject.exists():
        try:
            h.update(b"pyproject.toml")
            h.update(pyproject.read_bytes())
        except OSError as e:
            error(f"Failed to read pyproject.toml for fingerprint: {e!r}")
            h.update(b"ERROR:pyproject.toml")

    # backend file
    try:
        h.update(b"backend")
        h.update(BACKEND_PATH.read_bytes())
    except OSError as e:
        error(f"Failed to read backend file for fingerprint: {e!r}")
        h.update(b"ERROR:backend")

    # .pth file
    pth = PROJECT_ROOT / "src" / "_copium_autopatch.pth"
    if pth.exists():
        try:
            h.update(b"_copium_autopatch.pth")
            h.update(pth.read_bytes())
        except OSError as e:
            error(f"Failed to read _copium_autopatch.pth for fingerprint: {e!r}")
            h.update(b"ERROR:_copium_autopatch.pth")

    digest = h.hexdigest()
    echo(f"Computed sources fingerprint: {digest[:12]}... from {count} source files")
    return digest


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


def _get_c_extensions(
    version_info: dict[str, Any] | None = None,
    build_hash: str | None = None,
) -> list[Extension]:
    """Get C extensions with version baked in."""
    from setuptools import Extension

    python_include = Path(sysconfig.get_paths()["include"])

    # Get version info if not provided
    if version_info is None:
        version_info = _get_version_info(build_hash)

    # Convert to C macros
    define_macros = _version_info_to_macros(version_info)

    # Thread the build fingerprint into the native extension when available
    if build_hash is not None:
        # String-literal for the C preprocessor: "abcd1234..."
        define_macros.append(("COPIUM_BUILD_HASH", f'"{build_hash}"'))

    return [
        Extension(
            "copium",
            sources=["src/copium.c"],
            include_dirs=[str(python_include), str(python_include / "internal")],
            define_macros=define_macros,
            extra_compile_args=["/std:c11"] if sys.platform == "win32" else ["-std=c11"],
        )
    ]


# ============================================================================
# Extension Injection
# ============================================================================


def _wheel_fingerprint(
    build_type: str,
    config_settings: dict[str, Any] | None = None,
) -> str:
    """Compute fingerprint for wheel caching.

    This is designed to change whenever *anything that matters* changes:
    - Python/C sources in src/ (primary driver)
    - pyproject.toml
    - backend implementation (this file)
    - selected environment details and build command
    """
    fp = {
        "type": build_type,
        "sources": _sources_fingerprint(),
        "env": _environment_fingerprint(),
        "build_cmd": _build_command_fingerprint(config_settings),
    }
    fingerprint = _hash_dict(fp)
    echo(
        f"Build fingerprint for {build_type}: {fingerprint} (sources={str(fp['sources'])[:12]}...)"
    )
    return fingerprint


def _inject_extensions(
    build_type: str = "wheel",
    config_settings: dict[str, Any] | None = None,
) -> "Callable[..., Any]":
    """Monkey-patch setuptools.setup() to inject extensions, version, and build hash."""
    import setuptools

    original_setup = setuptools.setup

    # Compute build fingerprint to embed as COPIUM_BUILD_HASH and (optionally) in local version
    build_hash = _wheel_fingerprint(build_type, config_settings=config_settings)

    # Get version info once per build, with optional +build_hash local segment
    version_info = _get_version_info(build_hash)

    # Get extensions with version + build hash baked in
    extensions = _get_c_extensions(version_info, build_hash)

    def patched_setup(*args: Any, **kwargs: Any) -> Any:
        if extensions:
            kwargs.setdefault("ext_modules", []).extend(extensions)
        # Inject version from setuptools-scm (possibly overridden)
        kwargs.setdefault("version", version_info["version"])
        return original_setup(*args, **kwargs)

    setuptools.setup = patched_setup
    return original_setup


def _restore_setup(original: "Callable[..., Any]") -> None:
    """Restore original setuptools.setup()."""
    import setuptools

    setuptools.setup = original


# ============================================================================
# Cache Retention Policy
# ============================================================================


def _cleanup_cache() -> None:
    """
    Clean up old cache entries based on retention policy.

    Policy:
    - Keep last 5 builds per build type
    - Keep all builds from last 24 hours
    - Remove everything else
    """
    retention_hours = 24
    retention_count = 5
    now = time.time()
    cutoff_time = now - (retention_hours * 3600)

    def _cleanup_dir(cache_dir: Path, pattern: str) -> None:
        """Clean up a cache directory based on retention policy."""
        if not cache_dir.exists():
            return

        # Collect all cache entries with their modification times
        entries: list[tuple[Path, float]] = []
        for entry in cache_dir.iterdir():
            if entry.is_dir() and entry.name.startswith(pattern):
                try:
                    mtime = entry.stat().st_mtime
                    entries.append((entry, mtime))
                except OSError as e:
                    error(f"Failed to stat cache entry {entry}: {e!r}")
                    continue

        # Sort by modification time (newest first)
        entries.sort(key=lambda x: x[1], reverse=True)

        # Keep the newest N entries and anything within the time window
        for i, (entry, mtime) in enumerate(entries):
            # Keep if in top N or within time window
            if i < retention_count or mtime >= cutoff_time:
                continue

            # Remove old entry
            try:
                shutil.rmtree(entry)
                echo(f"Cleaned up old cache: {entry.name}")
            except OSError as e:
                error(f"Failed to remove {entry.name}: {e!r}")

    def _cleanup_requires_cache() -> None:
        """Clean up requires cache JSON files."""
        if not REQUIRES_CACHE.exists():
            return

        # Group files by build type
        by_type: dict[str, list[tuple[Path, float]]] = {}

        for entry in REQUIRES_CACHE.iterdir():
            if entry.is_file() and entry.suffix == ".json":
                try:
                    data = json.loads(entry.read_text())
                    timestamp = data.get("timestamp", 0.0)
                    build_type = entry.stem.split("-")[0]
                except NoExceptionError as e:
                    error(f"Failed to read requires cache {entry}: {e!r}")
                    continue

                by_type.setdefault(build_type, []).append((entry, timestamp))

        # Clean up each build type
        for build_type, entries in by_type.items():
            # Sort by timestamp (newest first)
            entries.sort(key=lambda x: x[1], reverse=True)

            for i, (entry, timestamp) in enumerate(entries):
                # Keep if in top N or within time window
                if i < retention_count or timestamp >= cutoff_time:
                    continue

                # Remove old entry
                try:
                    entry.unlink()
                    echo(f"Cleaned up old requires cache: {entry.name}")
                except OSError as e:
                    error(f"Failed to remove {entry.name}: {e!r}")

    # Clean up each cache type
    _cleanup_dir(WHEEL_CACHE, "wheel-")
    _cleanup_dir(WHEEL_CACHE, "editable-")
    _cleanup_dir(METADATA_CACHE, "wheel-")
    _cleanup_dir(METADATA_CACHE, "editable-")
    _cleanup_requires_cache()


def _ensure_cleanup_once() -> None:
    """
    Run cache cleanup if it hasn't been run in the last minute.

    Uses a filesystem timestamp file to coordinate across multiple build processes.
    """
    cleanup_marker = CACHE_ROOT / ".last_cleanup"
    cleanup_interval = 60  # seconds

    # Check if cleanup was recently performed
    if cleanup_marker.exists():
        try:
            last_cleanup = cleanup_marker.stat().st_mtime
            if time.time() - last_cleanup < cleanup_interval:
                # Cleanup was performed recently, skip
                return
        except OSError as e:
            error(f"Failed to stat cleanup marker: {e!r}")

    # Perform cleanup
    _cleanup_cache()

    # Update timestamp marker
    try:
        cleanup_marker.touch()
    except OSError as e:
        error(f"Failed to update cleanup marker: {e!r}")


# ============================================================================
# PEP 517 Backend Hooks
# ============================================================================


def get_requires_for_build_wheel(config_settings: dict[str, Any] | None = None) -> list[str]:
    """Get build requirements for wheel."""
    _ensure_cleanup_once()
    cache_key = f"wheel-{_project_fingerprint()}"
    cache_file = REQUIRES_CACHE / f"{cache_key}.json"

    # Check cache
    if cache_file.exists():
        try:
            cached = json.loads(cache_file.read_text())
            echo("Using cached build requirements")
            return cached["requires"]
        except NoExceptionError as e:
            error(f"Requires cache load failed: {e!r}")
            cache_file.unlink()

    # Generate
    echo("Getting build requirements...")
    result = setuptools_build_meta.get_requires_for_build_wheel(config_settings)

    # Cache
    try:
        cache_file.write_text(json.dumps({"requires": result, "timestamp": time.time()}, indent=2))
        echo("Cached build requirements")
    except NoExceptionError as e:
        error(f"Requires cache save failed: {e!r}")

    return result


def get_requires_for_build_editable(config_settings: dict[str, Any] | None = None) -> list[str]:
    """Get build requirements for editable install."""
    _ensure_cleanup_once()
    cache_key = f"editable-{_project_fingerprint()}"
    cache_file = REQUIRES_CACHE / f"{cache_key}.json"

    # Check cache
    if cache_file.exists():
        try:
            cached = json.loads(cache_file.read_text())
            echo("Using cached build requirements")
            return cached["requires"]
        except NoExceptionError as e:
            error(f"Requires cache load failed: {e!r}")
            cache_file.unlink()

    # Generate
    echo("Getting build requirements...")
    result = setuptools_build_meta.get_requires_for_build_editable(config_settings)

    # Cache
    try:
        cache_file.write_text(json.dumps({"requires": result, "timestamp": time.time()}, indent=2))
        echo("Cached build requirements")
    except NoExceptionError as e:
        error(f"Requires cache save failed: {e!r}")

    return result


def get_requires_for_build_sdist(config_settings: dict[str, Any] | None = None) -> list[str]:
    """Get build requirements for sdist."""
    cache_key = f"sdist-{_project_fingerprint()}"
    cache_file = REQUIRES_CACHE / f"{cache_key}.json"

    if cache_file.exists():
        try:
            cached = json.loads(cache_file.read_text())
            echo("Using cached build requirements (sdist)")
            return cached["requires"]
        except NoExceptionError as e:
            error(f"Requires cache load failed (sdist): {e!r}")
            cache_file.unlink()

    echo("Getting build requirements (sdist)...")
    result = setuptools_build_meta.get_requires_for_build_sdist(config_settings)

    # Cache
    try:
        cache_file.write_text(json.dumps({"requires": result, "timestamp": time.time()}, indent=2))
        echo("Cached build requirements (sdist)")
    except NoExceptionError as e:
        error(f"Requires cache save failed (sdist): {e!r}")

    return result


def prepare_metadata_for_build_wheel(
    metadata_directory: str, config_settings: dict[str, Any] | None = None
) -> str:
    """Prepare metadata for wheel build."""
    # Check session state
    if "wheel_metadata" in _session_state:
        cached_info = _session_state["wheel_metadata"]
        echo("Reusing metadata from session")
        src = Path(cached_info["path"])
        dist_info_name = cached_info["name"]
        dst = Path(metadata_directory) / dist_info_name
        _fast_copy_tree(src, dst)
        return dist_info_name

    # Check cross-session cache
    fingerprint = _wheel_fingerprint("wheel", config_settings=config_settings)
    cache_dir = METADATA_CACHE / f"wheel-{fingerprint}"
    info_file = cache_dir / ".dist-info-name"

    if cache_dir.exists() and info_file.exists():
        try:
            dist_info_name = info_file.read_text().strip()
            echo("Using cached metadata")
            dst = Path(metadata_directory) / dist_info_name
            _fast_copy_tree(cache_dir / dist_info_name, dst)
            _session_state["wheel_metadata"] = {
                "path": str(dst),
                "name": dist_info_name,
            }
        except NoExceptionError as e:
            error(f"Metadata cache load failed: {e!r}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return dist_info_name

    # Generate
    echo("Generating metadata...")
    original = _inject_extensions(build_type="wheel", config_settings=config_settings)
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
        echo("Cached metadata")
    except NoExceptionError as e:
        error(f"Metadata cache save failed: {e!r}")

    _session_state["wheel_metadata"] = {"path": str(src), "name": result}
    return result


def prepare_metadata_for_build_editable(
    metadata_directory: str, config_settings: dict[str, Any] | None = None
) -> str:
    """Prepare metadata for editable install."""
    # Check session state
    if "editable_metadata" in _session_state:
        cached_info = _session_state["editable_metadata"]
        echo("Reusing metadata from session")
        src = Path(cached_info["path"])
        dist_info_name = cached_info["name"]
        dst = Path(metadata_directory) / dist_info_name
        _fast_copy_tree(src, dst)
        return dist_info_name

    # Check cross-session cache
    fingerprint = _wheel_fingerprint("editable", config_settings=config_settings)
    cache_dir = METADATA_CACHE / f"editable-{fingerprint}"
    info_file = cache_dir / ".dist-info-name"

    if cache_dir.exists() and info_file.exists():
        try:
            dist_info_name = info_file.read_text().strip()
            echo("Using cached metadata")
            dst = Path(metadata_directory) / dist_info_name
            _fast_copy_tree(cache_dir / dist_info_name, dst)
            _session_state["editable_metadata"] = {
                "path": str(dst),
                "name": dist_info_name,
            }
        except NoExceptionError as e:
            error(f"Metadata cache load failed: {e!r}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return dist_info_name

    # Generate
    echo("Generating metadata...")
    original = _inject_extensions(build_type="editable", config_settings=config_settings)
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
        echo("Cached metadata")
    except NoExceptionError as e:
        error(f"Metadata cache save failed: {e!r}")

    _session_state["editable_metadata"] = {"path": str(src), "name": result}
    return result


def build_wheel(
    wheel_directory: str,
    config_settings: dict[str, Any] | None = None,
    metadata_directory: str | None = None,
) -> str:
    """Build a wheel."""
    _ensure_cleanup_once()

    if os.environ.get("COPIUM_DISABLE_WHEEL_CACHE") == "1":
        echo("Wheel caching disabled")
        original = _inject_extensions(build_type="wheel", config_settings=config_settings)
        try:
            return setuptools_build_meta.build_wheel(
                wheel_directory, config_settings, metadata_directory
            )
        finally:
            _restore_setup(original)

    # Check cache
    fingerprint = _wheel_fingerprint("wheel", config_settings=config_settings)
    cache_dir = WHEEL_CACHE / f"wheel-{fingerprint}"
    cache_file = cache_dir / "wheel.whl"
    name_file = cache_dir / ".wheel-name"

    if cache_file.exists() and name_file.exists():
        try:
            wheel_name = name_file.read_text().strip()
            echo("Using cached wheel (instant)")
            dst = Path(wheel_directory) / wheel_name
            _fast_copy(cache_file, dst)
        except NoExceptionError as e:
            error(f"Cache load failed: {e!r}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return wheel_name

    # Build
    echo("Building wheel...")
    original = _inject_extensions(build_type="wheel", config_settings=config_settings)
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
        echo("Cached wheel")
    except NoExceptionError as e:
        error(f"Cache save failed: {e!r}")

    return result


def build_sdist(sdist_directory: str, config_settings: dict[str, Any] | None = None) -> str:
    """Build a source distribution with version injection."""
    echo("Building sdist...")
    original = _inject_extensions(build_type="sdist", config_settings=config_settings)
    try:
        return setuptools_build_meta.build_sdist(sdist_directory, config_settings)
    finally:
        _restore_setup(original)


def build_editable(
    wheel_directory: str,
    config_settings: dict[str, Any] | None = None,
    metadata_directory: str | None = None,
) -> str:
    """Build an editable wheel."""
    _ensure_cleanup_once()

    if os.environ.get("COPIUM_DISABLE_WHEEL_CACHE") == "1":
        echo("Wheel caching disabled")
        original = _inject_extensions(build_type="editable", config_settings=config_settings)
        try:
            return setuptools_build_meta.build_editable(
                wheel_directory, config_settings, metadata_directory
            )
        finally:
            _restore_setup(original)

    # Check cache
    fingerprint = _wheel_fingerprint("editable", config_settings=config_settings)
    cache_dir = WHEEL_CACHE / f"editable-{fingerprint}"
    cache_file = cache_dir / "wheel.whl"
    name_file = cache_dir / ".wheel-name"

    if cache_file.exists() and name_file.exists():
        try:
            wheel_name = name_file.read_text().strip()
            echo("Using cached editable wheel (instant)")
            dst = Path(wheel_directory) / wheel_name
            _fast_copy(cache_file, dst)
        except NoExceptionError as e:
            error(f"Cache load failed: {e!r}")
            shutil.rmtree(cache_dir, ignore_errors=True)
        else:
            return wheel_name

    # Build
    echo("Building editable wheel...")
    original = _inject_extensions(build_type="editable", config_settings=config_settings)
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
        echo("Cached editable wheel")
    except NoExceptionError as e:
        error(f"Cache save failed: {e!r}")

    return result


def __getattr__(name: str) -> Any:
    from setuptools import build_meta

    return getattr(build_meta, name)
