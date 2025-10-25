# SPDX-FileCopyrightText: 2023-present Arseny
# SPDX-License-Identifier: MIT

from __future__ import annotations

import copy as stdlib_deepcopy

import copium.patch


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
        return "__copium_probe__", x

    # Sanity: original deepcopy should not return our marker
    assert stdlib_deepcopy.deepcopy(1) == 1

    try:
        copium.patch.apply(stdlib_deepcopy.deepcopy, probe_deepcopy)
        assert copium.patch.applied(stdlib_deepcopy.deepcopy)

        res = stdlib_deepcopy.deepcopy({"k": 7})
        assert res == ("__copium_probe__", {"k": 7})
        assert calls and isinstance(calls[-1], tuple)
        assert calls[-1][0] == {"k": 7}

        assert getattr(stdlib_deepcopy.deepcopy, "__wrapped__", None) is probe_deepcopy
    finally:
        copium.patch.unapply(stdlib_deepcopy.deepcopy)

    assert not copium.patch.applied(stdlib_deepcopy.deepcopy)
    assert not hasattr(stdlib_deepcopy.deepcopy, "__wrapped__")
    assert stdlib_deepcopy.deepcopy(1) == 1


def test_public_patch_api() -> None:
    """Test the public enable/disable/enabled API for patching copy.deepcopy."""
    # Ensure we start in a clean state
    if copium.patch.enabled():
        copium.patch.disable()

    assert not copium.patch.enabled(), "Should start unpatched"

    # Test enable()
    result = copium.patch.enable()
    assert result is True, "First enable() should return True"
    assert copium.patch.enabled(), "enabled() should return True after enable()"

    # Test that copy.deepcopy now uses copium
    test_obj = {"nested": [1, 2, 3], "key": "value"}
    copied = stdlib_deepcopy.deepcopy(test_obj)
    assert copied == test_obj
    assert copied is not test_obj

    # Test idempotent enable()
    result = copium.patch.enable()
    assert result is False, "Second enable() should return False (already patched)"
    assert copium.patch.enabled()

    # Test disable()
    result = copium.patch.disable()
    assert result is True, "First disable() should return True"
    assert not copium.patch.enabled(), "enabled() should return False after disable()"

    # Verify copy.deepcopy works normally after disable
    copied_after = stdlib_deepcopy.deepcopy(test_obj)
    assert copied_after == test_obj
    assert copied_after is not test_obj

    # Test idempotent disable()
    result = copium.patch.disable()
    assert result is False, "Second disable() should return False (already unpatched)"
    assert not copium.patch.enabled()


def test_public_patch_forwarding() -> None:
    """Verify that enabled patch actually forwards to copium.patch.deepcopy."""
    if copium.patch.enabled():
        copium.patch.disable()

    try:
        copium.patch.enable()

        # Create a custom class to verify copium's behavior
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
        copium.patch.disable()
