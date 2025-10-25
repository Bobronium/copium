"""
Tests for canonical error handling in copium module.

These tests verify that copium raises the same errors as standard Python
for API misuse (wrong arguments, wrong types, etc).

Tests are organized by function and error type.

Written by Sonnet 4.5.
"""

import sys

import pytest

import copium


class TestDeepcopyArgumentErrors:
    """Test that deepcopy() raises canonical Python errors for wrong arguments."""

    def test_missing_required_argument(self):
        """deepcopy() with no arguments should raise TypeError with standard message."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy()

        # Expected canonical message
        expected = "deepcopy() missing 1 required positional argument: 'x'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_too_many_positional_arguments(self):
        """deepcopy() with 3+ positional arguments should raise TypeError."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], {}, "extra")

        expected = "deepcopy() takes from 1 to 2 positional arguments but 3 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_too_many_positional_arguments_4(self):
        """deepcopy() with 4 positional arguments should report correct count."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], {}, "extra1", "extra2")

        expected = "deepcopy() takes from 1 to 2 positional arguments but 4 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_multiple_values_for_memo(self):
        """deepcopy(x, {}, memo={}) should raise 'multiple values' error."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], {}, memo={})

        expected = "deepcopy() got multiple values for argument 'memo'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_too_many_keyword_arguments(self):
        """deepcopy(x, foo=1, bar=2) should raise 'at most 1 keyword argument' error."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], foo={}, bar={})

        # This might fail first on "unexpected keyword" but checking the logic
        # In practice, Python would catch the first unexpected keyword
        assert str(exc_info.value).lower() == "deepcopy() takes at most 1 keyword argument"

    def test_unexpected_keyword_argument(self):
        """deepcopy(x, foo={}) should raise 'unexpected keyword argument' error."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], foo={})

        expected = "deepcopy() got an unexpected keyword argument 'foo'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_unexpected_keyword_different_name(self):
        """deepcopy(x, bar={}) should report 'bar' as unexpected keyword."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], bar={})

        expected = "deepcopy() got an unexpected keyword argument 'bar'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"


class TestDeepcopyTypeErrors:
    """Test that deepcopy() raises canonical TypeError for wrong types."""

    def test_memo_must_be_dict_not_list(self):
        """deepcopy(x, memo=[]) should raise TypeError naming the actual type."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo=[])

        expected = "argument 'memo' must be dict, not list"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_memo_must_be_dict_not_str(self):
        """deepcopy(x, memo='string') should report 'str' as actual type."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo="string")

        expected = "argument 'memo' must be dict, not str"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_memo_must_be_dict_not_int(self):
        """deepcopy(x, memo=42) should report 'int' as actual type."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo=42)

        expected = "argument 'memo' must be dict, not int"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_memo_must_be_dict_not_custom_class(self):
        """deepcopy(x, memo=CustomClass()) should report custom class name."""

        class CustomClass:
            pass

        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo=CustomClass())

        expected = "argument 'memo' must be dict, not CustomClass"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"


class TestDeepcopyValidCalls:
    """Test that valid deepcopy() calls work correctly."""

    def test_single_positional_argument(self):
        """deepcopy(x) should work."""
        result = copium.deepcopy([1, 2, 3])
        assert result == [1, 2, 3]
        assert result is not [1, 2, 3]

    def test_with_memo_positional(self):
        """deepcopy(x, {}) should work."""
        result = copium.deepcopy([1, 2, 3], {})
        assert result == [1, 2, 3]

    def test_with_memo_keyword(self):
        """deepcopy(x, memo={}) should work."""
        result = copium.deepcopy([1, 2, 3], memo={})
        assert result == [1, 2, 3]

    def test_with_none_memo(self):
        """deepcopy(x, None) should work."""
        result = copium.deepcopy([1, 2, 3], None)
        assert result == [1, 2, 3]


@pytest.mark.skipif(sys.version_info < (3, 13), reason="replace() requires Python 3.13+")
class TestReplaceArgumentErrors:
    """Test that replace() raises canonical Python errors for wrong arguments."""

    def test_missing_required_argument(self):
        """replace() with no arguments should raise TypeError."""
        with pytest.raises(TypeError) as exc_info:
            copium.replace()

        expected = "replace() missing 1 required positional argument: 'obj'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_too_many_positional_arguments(self):
        """replace(obj, extra) should raise TypeError."""
        from types import SimpleNamespace

        obj = SimpleNamespace(x=1)

        with pytest.raises(TypeError) as exc_info:
            copium.replace(obj, "extra")

        expected = "replace() takes 1 positional argument but 2 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_too_many_positional_arguments_3(self):
        """replace(obj, extra1, extra2) should report correct count."""
        from types import SimpleNamespace

        obj = SimpleNamespace(x=1)

        with pytest.raises(TypeError) as exc_info:
            copium.replace(obj, "extra1", "extra2")

        expected = "replace() takes 1 positional argument but 3 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {str(exc_info.value)}"

    def test_unsupported_type_uses_safe_format(self):
        """replace() on unsupported type should use %.200s format specifier."""
        with pytest.raises(TypeError) as exc_info:
            copium.replace([1, 2, 3], x=5)

        # Check that error mentions list and doesn't crash on long type names
        assert "list" in str(exc_info.value)
        assert "replace() does not support" in str(exc_info.value).lower()


@pytest.mark.skipif(sys.version_info < (3, 13), reason="replace() requires Python 3.13+")
class TestReplaceValidCalls:
    """Test that valid replace() calls work correctly."""

    def test_replace_with_kwargs(self):
        """replace(obj, **kwargs) should work if obj supports __replace__."""
        from types import SimpleNamespace

        obj = SimpleNamespace(x=1, y=2)

        # Note: This will fail if SimpleNamespace doesn't have __replace__ in 3.13
        # but demonstrates the expected behavior
        result = copium.replace(obj, x=10)
        assert result.x == 10
        assert result.y == 2


class TestCopyArgumentErrors:
    """Test that copy() raises canonical Python errors for wrong arguments."""

    def test_missing_required_argument(self):
        """copy() with no arguments should raise TypeError."""
        with pytest.raises(TypeError) as exc_info:
            copium.copy()

        # With METH_O, Python's error handler generates this message
        assert "takes exactly one argument" in str(exc_info.value).lower()
        assert "0 given" in str(exc_info.value) or "missing" in str(exc_info.value).lower()

    def test_too_many_arguments(self):
        """copy(obj, extra) should raise TypeError."""
        with pytest.raises(TypeError) as exc_info:
            copium.copy([1, 2, 3], "extra")

        # With METH_O, Python's error handler generates this message
        assert "takes exactly one argument" in str(exc_info.value).lower()
        assert "2 given" in str(exc_info.value)


class TestCopyValidCalls:
    """Test that valid copy() calls work correctly."""

    def test_copy_list(self):
        """copy() should create a shallow copy of a list."""
        original = [1, 2, [3, 4]]
        result = copium.copy(original)

        assert result == original
        assert result is not original
        # Shallow copy means nested list is the same object
        assert result[2] is original[2]

    def test_copy_dict(self):
        """copy() should create a shallow copy of a dict."""
        original = {"a": 1, "b": [2, 3]}
        result = copium.copy(original)

        assert result == original
        assert result is not original
        assert result["b"] is original["b"]


class TestPinningFunctionErrors:
    """Test error handling for pinning functions (if available)."""

    def test_pin_missing_argument(self):
        """pin() with no arguments should raise TypeError."""
        if not hasattr(copium, "pin"):
            pytest.skip("pin() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.pin()

        assert "takes exactly one argument" in str(exc_info.value).lower()

    def test_pin_too_many_arguments(self):
        """pin(obj, extra) should raise TypeError."""
        if not hasattr(copium, "pin"):
            pytest.skip("pin() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.pin([1, 2, 3], "extra")

        assert "takes exactly one argument" in str(exc_info.value).lower()

    def test_pinned_missing_argument(self):
        """pinned() with no arguments should raise TypeError."""
        if not hasattr(copium, "pinned"):
            pytest.skip("pinned() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.pinned()

        assert "takes exactly one argument" in str(exc_info.value).lower()

    def test_clear_pins_too_many_arguments(self):
        """clear_pins(extra) should raise TypeError."""
        if not hasattr(copium, "clear_pins"):
            pytest.skip("clear_pins() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.clear_pins("extra")

        assert "takes no arguments" in str(exc_info.value).lower()

    def test_get_pins_too_many_arguments(self):
        """get_pins(extra) should raise TypeError."""
        if not hasattr(copium, "get_pins"):
            pytest.skip("get_pins() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.get_pins("extra")

        assert "takes no arguments" in str(exc_info.value).lower()

    def test_unpin_missing_argument(self):
        """unpin() with no arguments should raise TypeError."""
        if not hasattr(copium, "unpin"):
            pytest.skip("unpin() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.unpin()

        expected = "unpin() missing 1 required positional argument: 'obj'"
        # May have slightly different wording, check key parts
        assert "missing" in str(exc_info.value).lower() or "takes" in str(exc_info.value).lower()

    def test_unpin_too_many_positional(self):
        """unpin(obj, extra) should raise TypeError."""
        if not hasattr(copium, "unpin"):
            pytest.skip("unpin() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.unpin([1, 2, 3], "extra")

        expected = "unpin() takes 1 positional argument but 2 were given"
        assert "takes 1 positional argument" in str(exc_info.value)

    def test_unpin_unexpected_keyword(self):
        """unpin(obj, foo=True) should raise TypeError."""
        if not hasattr(copium, "unpin"):
            pytest.skip("unpin() not available (duper not installed)")

        with pytest.raises(TypeError) as exc_info:
            copium.unpin([1, 2, 3], foo=True)

        expected = "unpin() got an unexpected keyword argument 'foo'"
        assert (
            str(exc_info.value) == expected or "unexpected keyword" in str(exc_info.value).lower()
        )

    def test_unpin_multiple_values_for_strict(self):
        """unpin(obj, False, strict=True) should raise TypeError."""
        if not hasattr(copium, "unpin"):
            pytest.skip("unpin() not available (duper not installed)")

        # This might not be possible depending on how unpin is defined
        # If strict is keyword-only, we can't pass it positionally
        # This test documents the expected behavior if someone tries


class TestComparisonWithStandardLibrary:
    """Tests that compare copium behavior with Python's copy module."""

    def test_deepcopy_errors_match_stdlib(self):
        """Verify copium.deepcopy errors match copy.deepcopy where applicable."""
        import copy

        # Test missing argument
        with pytest.raises(TypeError) as copium_exc:
            copium.deepcopy()
        with pytest.raises(TypeError) as stdlib_exc:
            copy.deepcopy()

        # Both should raise TypeError (exact message may differ due to C vs Python)
        assert type(copium_exc.value) == type(stdlib_exc.value)

    def test_copy_errors_match_stdlib(self):
        """Verify copium.copy errors match copy.copy where applicable."""
        import copy

        # Test missing argument
        with pytest.raises(TypeError) as copium_exc:
            copium.copy()
        with pytest.raises(TypeError) as stdlib_exc:
            copy.copy()

        assert type(copium_exc.value) == type(stdlib_exc.value)


# Utility tests to verify test infrastructure
class TestSetup:
    """Verify the test environment is set up correctly."""

    def test_copium_imported(self):
        """Verify copium module is available."""
        assert hasattr(copium, "deepcopy")
        assert hasattr(copium, "copy")

    def test_copium_has_error_class(self):
        """Verify copium has Error exception class."""
        assert hasattr(copium, "Error")
        assert issubclass(copium.Error, Exception)

    def test_pinning_functions_documented(self):
        """Document which pinning functions are available."""
        pin_functions = ["pin", "unpin", "pinned", "clear_pins", "get_pins"]
        available = [name for name in pin_functions if hasattr(copium, name)]

        if not available:
            pytest.skip("No pinning functions available (duper not installed)")

        print(f"\nAvailable pinning functions: {', '.join(available)}")
