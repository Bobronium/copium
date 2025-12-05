# SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
#
# SPDX-License-Identifier: MIT
from __future__ import annotations

import ast
import copy as stdlib_copy
import inspect
import marshal
import os
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from _pytest.assertion.rewrite import rewrite_asserts

import copium
from datamodelzoo import CASES
from datamodelzoo import EVIL_CASES

if TYPE_CHECKING:
    from types import FunctionType


def pytest_addoption(parser):
    """Add custom command line options."""
    parser.addoption(
        "--memory",
        action="store_true",
        default=False,
        help="Run memory leak tests (slow, requires psutil)",
    )


def pytest_configure(config):
    config.option.snapshot_dirname = f".expected/{sys.version_info.major}.{sys.version_info.minor}/"
    config.option.snapshot_patch_pycharm_diff = True
    # Register custom markers
    config.addinivalue_line("markers", "memory: mark test as memory leak test (opt-in)")
    config.addinivalue_line(
        "markers",
        "subprocess(environ=None): run the body of the test in a fresh Python subprocess",
    )


def pytest_collection_modifyitems(config, items):
    """Skip memory tests unless --memory flag is provided."""
    if not config.getoption("--memory"):
        skip_memory = pytest.mark.skip(reason="need --memory option to run")
        for item in items:
            if "memory" in item.keywords:
                item.add_marker(skip_memory)


CASE_PARAMS = [case.as_pytest_param() for case in CASES]
EVIL_CASE_PARAMS = [case.as_pytest_param() for case in EVIL_CASES]


class CopyModule:  # just for typing
    error = Error = copium.Error
    copy = staticmethod(copium.copy)
    deepcopy = staticmethod(copium.deepcopy)
    if sys.version_info >= (3, 13):
        replace = staticmethod(copium.replace)


@pytest.fixture(
    params=[
        pytest.param(stdlib_copy, id="stdlib"),
        pytest.param(copium, id="copium"),
        # sanity check
    ]
)
def copy(request) -> CopyModule:
    return request.param


def _get_function_body_source_and_first_lineno(function: FunctionType) -> tuple[str, int]:
    """
    Return (dedented_body_source, first_body_lineno_in_original_file).

    Unlike _extract_source_code_from_function, this also tells us
    which original line the first body line lives on, so we can
    align tracebacks.
    """
    source_lines, start_lineno = inspect.getsourcelines(function)

    body_started = False
    body_first_lineno: int | None = None
    body_lines: list[str] = []

    for offset, line in enumerate(source_lines):
        if not body_started:
            # skip decorators etc until we hit the def
            if line.lstrip().startswith("def "):
                body_started = True
            continue

        if body_first_lineno is None:
            body_first_lineno = start_lineno + offset
        body_lines.append(line)

    if body_first_lineno is None:
        # No body (e.g. "pass") – treat as empty.
        body_first_lineno = start_lineno + len(source_lines)
        body_lines = []

    body_source = textwrap.dedent("".join(body_lines))
    return body_source, body_first_lineno


def _ensure_subprocess_safe_function(func: FunctionType) -> None:
    """
    Ensure that a function used with @pytest.mark.subprocess:
    - takes no arguments (including *args/**kwargs)
    - has no free variables (i.e. doesn't close over outer scope)
    """
    code = func.__code__

    posonly = getattr(code, "co_posonlyargcount", 0)
    argcount = code.co_argcount
    kwonly = code.co_kwonlyargcount
    flags = code.co_flags

    if posonly or argcount or kwonly or (flags & (inspect.CO_VARARGS | inspect.CO_VARKEYWORDS)):
        raise RuntimeError(
            f"@pytest.mark.subprocess function {func.__qualname__} must not accept any arguments "
            f"(posonly={posonly}, args={argcount}, kwonly={kwonly}, "
            f"varargs/kwargs={bool(flags & (inspect.CO_VARARGS | inspect.CO_VARKEYWORDS))})"
        )

    if code.co_freevars:
        raise RuntimeError(
            f"@pytest.mark.subprocess function {func.__qualname__} must not close over outer-scope variables "
            f"(freevars={code.co_freevars!r})"
        )


def _apply_rewrite_asserts(tree: ast.AST, source: str, original_path: str) -> None:
    """
    Call pytest's rewrite_asserts with whatever signature the installed pytest uses.
    """
    try:
        sig = inspect.signature(rewrite_asserts)
        argc = len(sig.parameters)
    except Exception:
        # Fallback: just call it with the tree only.
        rewrite_asserts(tree)
        return

    src_bytes = source.encode("utf-8")

    if argc == 1:
        # rewrite_asserts(tree)
        rewrite_asserts(tree)
    elif argc == 2:
        # rewrite_asserts(tree, source)
        rewrite_asserts(tree, src_bytes)
    else:
        # rewrite_asserts(tree, source, module_path)
        rewrite_asserts(tree, src_bytes, original_path)


def _compile_rewritten_subprocess_body(
    body_source: str,
    original_path: str,
    body_first_lineno: int,
):
    """
    Create a code object for the body which:
    - has pytest-style rewritten asserts
    - reports filename = original_path
    - reports line numbers starting at body_first_lineno
    """
    # Align the body so that its first line has lineno == body_first_lineno.
    padded_source = ("\n" * (body_first_lineno - 1)) + body_source.lstrip("\n")

    tree = ast.parse(padded_source, filename=original_path)
    _apply_rewrite_asserts(tree, padded_source, original_path)
    return compile(tree, original_path, "exec")


@pytest.hookimpl(tryfirst=True)
def pytest_pyfunc_call(pyfuncitem):
    """
    Intercept tests marked with @pytest.mark.subprocess and run only their body
    in a fresh Python subprocess.

    - Assertions are rewritten using pytest's machinery *before* spawning.
    - Tracebacks point at the original test file and body line numbers.
    """
    mark = pyfuncitem.get_closest_marker("subprocess")
    if mark is None:
        return None

    func: FunctionType = pyfuncitem.obj

    _ensure_subprocess_safe_function(func)

    body_source, body_first_lineno = _get_function_body_source_and_first_lineno(func)
    if not body_source.strip():
        raise RuntimeError(f"No code was extracted fron {func}")

    # IMPORTANT: use an absolute path for the compiled code's filename.
    # Using a relative path breaks tools that re-open the source file
    # when the subprocess cwd is a tmp directory.
    path_obj = getattr(pyfuncitem, "path", None)
    if path_obj is not None:
        # pytest >= 7: path is a pathlib.Path
        original_path = str(Path(path_obj).resolve())
    else:
        # Older pytest: fall back to location[0]
        original_path_str, _def_lineno, _ = pyfuncitem.location
        original_path = str(Path(original_path_str).resolve())

    code = _compile_rewritten_subprocess_body(
        body_source=body_source,
        original_path=original_path,
        body_first_lineno=body_first_lineno,
    )

    code_bytes = marshal.dumps(code)

    tmp_path = pyfuncitem._request.getfixturevalue("tmp_path")
    runner_file = tmp_path / f"{pyfuncitem.name}_subprocess_runner.py"

    runner_source = f"""\
import marshal

_code_bytes = {code_bytes!r}
_code = marshal.loads(_code_bytes)
_globals = {{'__name__': '__main__'}}
exec(_code, _globals)
"""
    runner_file.write_text(runner_source)

    env = mark.kwargs.get("environ", os.environ)

    proc = subprocess.run(
        [sys.executable, str(runner_file)],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
    )
    output = proc.stdout + proc.stderr
    if proc.returncode != 0:
        pytest.fail(
            f"subprocess run failed with non-zero exit code {proc.returncode}:\n\n{output}",
            pytrace=False,
        )

    # We handled execution ourselves; pytest must not call the test function again.
    return True