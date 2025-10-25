from __future__ import annotations

import inspect
import types
from pathlib import Path
from typing import Any

import copium

PACKAGE_ROOT = Path(copium.__file__).parent / "copium"
PACKAGE_STUB = PACKAGE_ROOT / "__init__.pyi"


def _exec_stub_module(path: Path) -> types.ModuleType:
    """
    Execute the .pyi stub as if it were a normal Python module to get callable objects
    with inspectable signatures. This works because function bodies contain docstrings/ellipsis.
    """
    code = path.read_text(encoding="utf-8")
    module = types.ModuleType("copium_stub_exec")
    module.__file__ = str(path)
    # Give stubs a minimal environment; they import typing/sys already.
    namespace: dict[str, Any] = module.__dict__
    exec(compile(code, str(path), "exec"), namespace, namespace)
    return module


def _drop_clinic_internals(signature: inspect.Signature) -> inspect.Signature:
    """
    Remove leading Argument Clinic internal module/self params from a builtin signature.
    These appear as positional-only parameters named '$module', 'module', '$self', or 'self'.
    """
    params = list(signature.parameters.values())
    filtered: list[inspect.Parameter] = []
    skip_names = {"$module", "module", "$self", "self"}
    for i, parameter in enumerate(params):
        if (
            i == 0
            and parameter.kind is inspect.Parameter.POSITIONAL_ONLY
            and parameter.name in skip_names
        ):
            # skip the implicit module/self
            continue
        filtered.append(parameter)
    return signature.replace(parameters=tuple(filtered))


def _minimal_args_from_signature(signature: inspect.Signature) -> tuple[list[Any], dict[str, Any]]:
    """
    Build the minimal (args, kwargs) required to satisfy the given signature.
    - Positional-only and positional-or-keyword without defaults: add positional dummy.
    - Keyword-only without defaults: add keyword dummy.
    - *args/**kwargs are optional and may be empty.
    """
    args: list[Any] = []
    kwargs: dict[str, Any] = {}
    sentinel = object()

    for parameter in signature.parameters.values():
        if parameter.kind in (
            inspect.Parameter.POSITIONAL_ONLY,
            inspect.Parameter.POSITIONAL_OR_KEYWORD,
        ):
            if parameter.default is inspect._empty:
                args.append(sentinel)
        elif (
            parameter.kind is inspect.Parameter.KEYWORD_ONLY and parameter.default is inspect._empty
        ):
            kwargs[parameter.name] = sentinel
        # VAR_POSITIONAL/VAR_KEYWORD require nothing minimally
    return args, kwargs


def _collect_stub_functions(stub_module: types.ModuleType) -> dict[str, Any]:
    """
    Gather top-level functions defined in the stub module.
    Ignore dunders, TypeVars, and imported names that are not functions.
    """
    functions: dict[str, Any] = {}
    for name, obj in vars(stub_module).items():
        if name.startswith("__"):
            continue
        if inspect.isfunction(obj):
            functions[name] = obj
    return functions


def test_stub_signatures_bind_to_runtime_signatures(subtests):
    assert PACKAGE_STUB.exists(), f"Stub file not found at {PACKAGE_STUB}"
    stub_module = _exec_stub_module(PACKAGE_STUB)
    stub_functions = _collect_stub_functions(stub_module)

    for name, stub_function in sorted(stub_functions.items()):
        with subtests.test(name):
            runtime_object = getattr(copium, name)
            # Get signatures
            stub_signature = inspect.signature(stub_function)
            runtime_signature_raw = inspect.signature(runtime_object)
            runtime_signature = _drop_clinic_internals(runtime_signature_raw)

            # Build minimal args/kwargs from the stub (ignoring annotations entirely)
            args, kwargs = _minimal_args_from_signature(stub_signature)

            # Now ensure runtime can bind these minimal arguments
            try:
                runtime_signature.bind(*args, **kwargs)
            except TypeError as e:
                raise AssertionError(
                    f"{name}: runtime cannot bind minimal stub call.\n"
                    f"  stub signature: {stub_signature}\n"
                    f"  runtime signature: {runtime_signature}\n"
                    f"  args={len(args)} kwargs={list(kwargs.keys())}\n"
                    f"  error: {e}"
                )
