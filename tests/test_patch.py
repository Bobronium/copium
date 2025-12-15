import copy
import sys

import copy as stdlib_copy
import copium.patch


COPIED = {"a": [1, 2]}


def trace_calls():
    """Returns (result, list of (module, qualname) tuples)."""
    calls = []

    def tracer(frame, event, arg):
        if event == "call":
            code = frame.f_code
            module = frame.f_globals.get("__name__", "")
            calls.append((module, code.co_name))
        return tracer

    old = sys.gettrace()
    sys.settrace(tracer)
    try:
        stdlib_copy.deepcopy(COPIED)
    finally:
        sys.settrace(old)
    return calls


STDLIB_EXCLUSIVE_CALLS = {
    ("copy", "_deepcopy_list"),
    ("copy", "_deepcopy_dict"),
}
if sys.version_info >= (3, 12):
    # on <3.12, there will be one frame with copy.deepcopy on stack
    STDLIB_EXCLUSIVE_CALLS.add(("copy", "deepcopy"))


def test_unpatched_has_stdlib_internals():
    copium.patch.disable()
    calls = trace_calls()
    assert set(calls) & STDLIB_EXCLUSIVE_CALLS


def test_patched_no_stdlib_internals():
    copium.patch.enable()
    calls = trace_calls()
    assert not (set(calls) & STDLIB_EXCLUSIVE_CALLS)
    copium.patch.disable()


def test_disable_restores_stdlib_internals():
    copium.patch.disable()
    before_enabled = trace_calls()

    copium.patch.enable()
    when_enabled = trace_calls()

    copium.patch.disable()
    after_disabled = trace_calls()

    assert set(before_enabled) & STDLIB_EXCLUSIVE_CALLS
    assert not (set(when_enabled) & STDLIB_EXCLUSIVE_CALLS)
    assert set(after_disabled) & STDLIB_EXCLUSIVE_CALLS


def test_no_copium_attributes_after_disable():
    copium.patch.enable()
    copium.patch.disable()

    fn = copy.deepcopy
    assert not hasattr(fn, "__copium_original__")
    assert not hasattr(fn, "__wrapped__")


def test_idempotent_enable():
    copium.patch.disable()
    assert copium.patch.enable() == True
    assert copium.patch.enable() == False
    assert copium.patch.enabled() == True
    copium.patch.disable()


def test_idempotent_disable():
    copium.patch.enable()
    assert copium.patch.disable() == True
    assert copium.patch.disable() == False
    assert copium.patch.enabled() == False
