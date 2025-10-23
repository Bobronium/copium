# SPDX-FileCopyrightText: 2023-present Arseny
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy as stdlib_deepcopy

import copyc.patch


def test_patch_copy_deepcopy() -> None:
    """:
    - apply(copy.deepcopy, target) forwards all calls to `target`
    - applied(...) reflects True during the patch
    - unpatch(...) restores the original function behavior
    """
    calls: list[tuple[object, object | None]] = []

    def probe_deepcopy(x, memo=None):
        calls.append((x, memo))
        # Return a distinct marker so we know this path executed
        return "__copyc_probe__", x

    # Sanity: original deepcopy should not return our marker
    assert stdlib_deepcopy.deepcopy(1) == 1

    try:
        copyc.patch.apply(stdlib_deepcopy.deepcopy, probe_deepcopy)
        assert copyc.patch.applied(stdlib_deepcopy.deepcopy)

        res = stdlib_deepcopy.deepcopy({"k": 7})
        assert res == ("__copyc_probe__", {"k": 7})
        assert calls and isinstance(calls[-1], tuple)
        assert calls[-1][0] == {"k": 7}

        assert getattr(stdlib_deepcopy.deepcopy, "__wrapped__", None) is probe_deepcopy
    finally:
        copyc.patch.unapply(stdlib_deepcopy.deepcopy)

    assert not copyc.patch.applied(stdlib_deepcopy.deepcopy)
    assert not hasattr(stdlib_deepcopy.deepcopy, "__wrapped__")
    assert stdlib_deepcopy.deepcopy(1) == 1


def test_public_patch_api() -> None:
    """Test the public enable/disable/enabled API for patching copy.deepcopy."""
    # Ensure we start in a clean state
    if copyc.patch.enabled():
        copyc.patch.disable()

    assert not copyc.patch.enabled(), "Should start unpatched"

    # Test enable()
    result = copyc.patch.enable()
    assert result is True, "First enable() should return True"
    assert copyc.patch.enabled(), "enabled() should return True after enable()"

    # Test that copy.deepcopy now uses copyc
    test_obj = {"nested": [1, 2, 3], "key": "value"}
    copied = stdlib_deepcopy.deepcopy(test_obj)
    assert copied == test_obj
    assert copied is not test_obj

    # Test idempotent enable()
    result = copyc.patch.enable()
    assert result is False, "Second enable() should return False (already patched)"
    assert copyc.patch.enabled()

    # Test disable()
    result = copyc.patch.disable()
    assert result is True, "First disable() should return True"
    assert not copyc.patch.enabled(), "enabled() should return False after disable()"

    # Verify copy.deepcopy works normally after disable
    copied_after = stdlib_deepcopy.deepcopy(test_obj)
    assert copied_after == test_obj
    assert copied_after is not test_obj

    # Test idempotent disable()
    result = copyc.patch.disable()
    assert result is False, "Second disable() should return False (already unpatched)"
    assert not copyc.patch.enabled()


def test_public_patch_forwarding() -> None:
    """Verify that enabled patch actually forwards to copyc.patch.deepcopy."""
    if copyc.patch.enabled():
        copyc.patch.disable()

    try:
        copyc.patch.enable()

        # Create a custom class to verify copyc's behavior
        class CustomClass:
            def __init__(self, value):
                self.value = value

            def __eq__(self, other):
                return isinstance(other, CustomClass) and self.value == other.value

        original = CustomClass(42)
        copied = stdlib_deepcopy.deepcopy(original)

        assert copied == original
        assert copied is not original
        assert isinstance(copied, CustomClass)

    finally:
        copyc.patch.disable()
