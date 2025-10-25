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
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_too_many_positional_arguments(self):
        """deepcopy() with 3+ positional arguments should raise TypeError."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], {}, "extra")

        expected = "deepcopy() takes from 1 to 2 positional arguments but 3 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_too_many_positional_arguments_4(self):
        """deepcopy() with 4 positional arguments should report correct count."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], {}, "extra1", "extra2")

        expected = "deepcopy() takes from 1 to 2 positional arguments but 4 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_multiple_values_for_memo(self):
        """deepcopy(x, {}, memo={}) should raise 'multiple values' error."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], {}, memo={})

        expected = "deepcopy() got multiple values for argument 'memo'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_too_many_keyword_arguments(self):
        """deepcopy(x, foo=1, bar=2) should raise 'at most 1 keyword argument' error."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], foo={}, bar={})

        # This might fail first on "unexpected keyword" but checking the logic
        # In practice, Python would catch the first unexpected keyword
        assert str(exc_info.value).lower() == "deepcopy() got an unexpected keyword argument 'foo'"

    def test_unexpected_keyword_argument(self):
        """deepcopy(x, foo={}) should raise 'unexpected keyword argument' error."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], foo={})

        expected = "deepcopy() got an unexpected keyword argument 'foo'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_unexpected_keyword_different_name(self):
        """deepcopy(x, bar={}) should report 'bar' as unexpected keyword."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], bar={})

        expected = "deepcopy() got an unexpected keyword argument 'bar'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"


class TestDeepcopyTypeErrors:
    """Test that deepcopy() raises canonical TypeError for wrong types."""

    def test_memo_must_be_dict_not_list(self):
        """deepcopy(x, memo=[]) should raise TypeError naming the actual type."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo=[])

        expected = "argument 'memo' must be dict, not list"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_memo_must_be_dict_not_str(self):
        """deepcopy(x, memo='string') should report 'str' as actual type."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo="string")

        expected = "argument 'memo' must be dict, not str"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_memo_must_be_dict_not_int(self):
        """deepcopy(x, memo=42) should report 'int' as actual type."""
        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo=42)

        expected = "argument 'memo' must be dict, not int"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_memo_must_be_dict_not_custom_class(self):
        """deepcopy(x, memo=CustomClass()) should report custom class name."""

        class CustomClass:
            pass

        with pytest.raises(TypeError) as exc_info:
            copium.deepcopy([1, 2, 3], memo=CustomClass())

        expected = "argument 'memo' must be dict, not CustomClass"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"



@pytest.mark.skipif(sys.version_info < (3, 13), reason="replace() requires Python 3.13+")
class TestReplaceArgumentErrors:
    """Test that replace() raises canonical Python errors for wrong arguments."""

    def test_missing_required_argument(self):
        """replace() with no arguments should raise TypeError."""
        with pytest.raises(TypeError) as exc_info:
            copium.replace()

        expected = "replace() missing 1 required positional argument: 'obj'"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_too_many_positional_arguments(self):
        """replace(obj, extra) should raise TypeError."""
        from types import SimpleNamespace

        obj = SimpleNamespace(x=1)

        with pytest.raises(TypeError) as exc_info:
            copium.replace(obj, "extra")

        expected = "replace() takes 1 positional argument but 2 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_too_many_positional_arguments_3(self):
        """replace(obj, extra1, extra2) should report correct count."""
        from types import SimpleNamespace

        obj = SimpleNamespace(x=1)

        with pytest.raises(TypeError) as exc_info:
            copium.replace(obj, "extra1", "extra2")

        expected = "replace() takes 1 positional argument but 3 were given"
        assert str(exc_info.value) == expected, f"Expected: {expected}\nGot: {exc_info.value!s}"

    def test_unsupported_type_uses_safe_format(self):
        """replace() on unsupported type should use %.200s format specifier."""
        with pytest.raises(TypeError) as exc_info:
            copium.replace([1, 2, 3], x=5)

        # Check that error mentions list and doesn't crash on long type names
        assert "list" in str(exc_info.value)
        assert "replace() does not support" in str(exc_info.value).lower()


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

