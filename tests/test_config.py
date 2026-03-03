"""
Tests for copium configuration: both programmatic (configure/get_config) and env vars.
"""

from __future__ import annotations

import os
import re
import warnings
from pathlib import Path
from typing import Any
from typing import ClassVar

import pytest

import copium
import copium.patch
from tests.conftest import COPIUM_ENV

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def env(**kwargs):
    """Build a clean env dict for subprocess tests."""
    return {**COPIUM_ENV, **kwargs}


class Stubborn:
    """A type whose __deepcopy__ rejects non-dict memo."""

    def __deepcopy__(self, memo):
        if type(memo) is not dict:
            raise TypeError("memo must be a dict")
        return Stubborn()


class StubbornTracking:
    """Like Stubborn, but records memo objects for inspection."""

    memos: ClassVar[list[Any]] = []

    def __deepcopy__(self, memo):
        StubbornTracking.memos.append(memo)
        if type(memo) is not dict:
            raise TypeError("memo must be a dict")
        return StubbornTracking()


# ===========================================================================
#  get_config()
# ===========================================================================


class TestGetConfig:
    def test_returns_dict_with_expected_keys(self):
        cfg = copium.get_config()
        assert set(cfg) == {"memo", "on_incompatible", "suppress_warnings"}

    def test_default_values(self):
        copium.configure()
        cfg = copium.get_config()
        assert cfg["memo"] == "native"
        assert cfg["on_incompatible"] == "warn"
        assert cfg["suppress_warnings"] == ()


# ===========================================================================
#  configure() — basic API
# ===========================================================================


class TestConfigureAPI:
    def test_no_positional_args(self):
        with pytest.raises(TypeError, match="no positional arguments"):
            copium.configure("native")  # type: ignore[misc]

    def test_unexpected_kwarg(self):
        with pytest.raises(TypeError, match="unexpected keyword argument"):
            copium.configure(bogus="x")  # type: ignore[call-arg]

    def test_invalid_memo_value(self):
        with pytest.raises(ValueError, match="'native' or 'dict'"):
            copium.configure(memo="fast")  # type: ignore[arg-type]

    def test_invalid_on_incompatible_value(self):
        with pytest.raises(ValueError, match="'warn', 'raise', or 'silent'"):
            copium.configure(on_incompatible="ignore")  # type: ignore[arg-type]

    def test_suppress_warnings_non_string_item(self):
        with pytest.raises(
            TypeError, match=re.escape("on_incompatible[0] must be a 'str', got 'int'")
        ):
            copium.configure(suppress_warnings=[42])  # type: ignore[list-item]

    def test_suppress_warnings_non_iterable(self):
        with pytest.raises(TypeError):
            copium.configure(suppress_warnings=42)  # type: ignore[arg-type]


# ===========================================================================
#  configure() — memo modes
# ===========================================================================


class TestConfigureMemo:
    def test_memo_dict(self):
        copium.configure(memo="dict")
        assert copium.get_config()["memo"] == "dict"

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = copium.deepcopy(Stubborn())

        assert isinstance(result, Stubborn)
        assert not w

    def test_memo_native(self):
        copium.configure(memo="native")
        assert copium.get_config()["memo"] == "native"

    def test_explicit_dict_memo_arg_bypasses_config(self):
        """deepcopy(obj, {}) always uses dict memo regardless of config."""
        copium.configure(memo="native")
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = copium.deepcopy(Stubborn(), {})

        assert isinstance(result, Stubborn)
        assert not w


# ===========================================================================
#  configure() — on_incompatible
# ===========================================================================


class TestConfigureOnIncompatible:
    def test_raise(self):
        copium.configure(on_incompatible="raise")

        with pytest.raises(TypeError, match="memo must be a dict"):
            copium.deepcopy(Stubborn())

    def test_silent(self):
        copium.configure(on_incompatible="silent")

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = copium.deepcopy(Stubborn())

        assert isinstance(result, Stubborn)
        assert not w

    def test_warn(self):
        copium.configure(on_incompatible="warn")

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = copium.deepcopy(Stubborn())

        assert isinstance(result, Stubborn)
        assert len(w) == 1
        assert issubclass(w[0].category, UserWarning)

    def test_irrelevant_when_memo_dict(self):
        """on_incompatible has no effect when memo='dict' — situation never arises."""
        with pytest.raises(TypeError):
            copium.configure(memo="dict", on_incompatible="raise")

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = copium.deepcopy(Stubborn())

        assert isinstance(result, Stubborn)
        assert not w


# ===========================================================================
#  configure() — suppress_warnings
# ===========================================================================


class TestConfigureSuppressWarnings:
    def test_matching_suppression(self):
        copium.configure(suppress_warnings=["TypeError: memo must be a dict"])

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = copium.deepcopy(Stubborn())

        assert isinstance(result, Stubborn)
        assert not w

    def test_non_matching_suppression(self):
        copium.configure(suppress_warnings=["TypeError: different error"])

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            copium.deepcopy(Stubborn())

        assert len(w) == 1

    def test_none_clears(self):
        copium.configure(suppress_warnings=["TypeError: memo must be a dict"])
        assert copium.get_config()["suppress_warnings"] == ("TypeError: memo must be a dict",)

        copium.configure(suppress_warnings=None)
        assert copium.get_config()["suppress_warnings"] == ()

    def test_empty_clears(self):
        copium.configure(suppress_warnings=["something"])
        copium.configure(suppress_warnings=())
        assert copium.get_config()["suppress_warnings"] == ()


# ===========================================================================
#  configure() — incremental behavior
# ===========================================================================


class TestConfigureIncremental:
    def test_setting_memo_preserves_on_incompatible(self):
        copium.configure(on_incompatible="silent")
        copium.configure(memo="dict")
        assert copium.get_config()["on_incompatible"] == "silent"

    def test_setting_on_incompatible_preserves_memo(self):
        copium.configure(memo="dict")
        copium.configure(on_incompatible="raise")
        assert copium.get_config()["memo"] == "dict"

    def test_settings_persist_across_memo_switch(self):
        """on_incompatible='silent' survives memo round-trip."""
        copium.configure(memo="native", on_incompatible="silent")
        copium.configure(memo="dict")
        copium.configure(memo="native")
        assert copium.get_config()["on_incompatible"] == "silent"


# ===========================================================================
#  configure() — reset
# ===========================================================================


class TestConfigureReset:
    def test_reset_restores_defaults(self):
        copium.configure(memo="dict")
        copium.configure()
        cfg = copium.get_config()
        assert cfg["memo"] == "native"
        assert cfg["on_incompatible"] == "warn"
        assert cfg["suppress_warnings"] == ()

    def test_reset_restores_to_current_env(self):
        os.environ["COPIUM_USE_DICT_MEMO"] = "1"
        copium.configure()
        assert copium.get_config()["memo"] == "dict"


# ===========================================================================
#  Warning message format
# ===========================================================================


class TestWarningMessage:
    def test_contains_configure_suggestions(self):
        copium.configure(memo="native", on_incompatible="warn")

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            copium.deepcopy(Stubborn())

        assert len(w) == 1
        message = str(w[0].message)
        assert "'copium.memo' was rejected inside" in message
        assert "should treat memo as an opaque object" in message
        assert 'copium.configure(memo="dict")' in message
        assert 'copium.configure(on_incompatible="raise")' in message
        assert "copium.configure(suppress_warnings=[" in message
        assert "COPIUM_USE_DICT_MEMO=1" in message


# ===========================================================================
#  Environment variable tests (subprocess — needed for import-time behavior)
# ===========================================================================


@pytest.mark.subprocess(environ=env())
def test_env_memo_fallback_warning():
    import warnings

    import copium

    memos: list[Any] = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    def assert_warning_message(caught_warnings, deepcopy_expr: str, call_site_line: str):
        assert len(caught_warnings) == 1, f"Expected 1 warning, got {len(caught_warnings)}"
        message = str(caught_warnings[0].message)
        assert issubclass(caught_warnings[0].category, UserWarning)

        assert f"'copium.memo' was rejected inside '{__name__}.A.__deepcopy__'" in message
        assert 'raise TypeError("memo must be a dict")' in message
        assert (
            f"Per Python docs, '{__name__}.A.__deepcopy__' should treat memo as an opaque object."
            in message
        )
        assert call_site_line in message

        deepcopy_expr_with_memo = deepcopy_expr[:-1].rstrip(",") + ", memo={})"
        assert f"change {deepcopy_expr} to {deepcopy_expr_with_memo}" in message

        assert "copium.configure(suppress_warnings=[" in message
        assert 'copium.configure(memo="dict")' in message
        assert 'copium.configure(on_incompatible="raise")' in message
        assert "COPIUM_USE_DICT_MEMO=1" in message

    def assert_memo_replaced(_memos: list[Any]):
        assert len(_memos) == 2
        assert type(_memos[0]) is not dict
        assert type(_memos[1]) is dict
        _memos.clear()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        __result = copium.deepcopy(A())

    assert_warning_message(
        w,
        deepcopy_expr="copium.deepcopy(A())",
        call_site_line="__result = copium.deepcopy(A())",
    )
    assert_memo_replaced(memos)

    def identity(x):
        return x

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = identity(copium.deepcopy(A()))

    assert_warning_message(
        w,
        deepcopy_expr="copium.deepcopy(A())",
        call_site_line="_result = identity(copium.deepcopy(A()))",
    )
    assert_memo_replaced(memos)

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = copium.deepcopy(
            A(),
        )

    assert_warning_message(
        w,
        deepcopy_expr="deepcopy(A())",
        call_site_line="_result = copium.deepcopy(",
    )
    assert_memo_replaced(memos)

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        assert copium.deepcopy(A()) is not None

    assert_warning_message(
        w,
        deepcopy_expr="copium.deepcopy(A())",
        call_site_line="assert copium.deepcopy(A()) is not None",
    )
    assert_memo_replaced(memos)

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        # fmt: off
        _result = copium.deepcopy(A(),)  # noqa: COM819
        # fmt: on

    assert_warning_message(
        w, "copium.deepcopy(A(),)", "_result = copium.deepcopy(A(),)  # noqa: COM819"
    )
    assert_memo_replaced(memos)


@pytest.mark.subprocess(environ=env())
def test_env_memo_fallback_warning_aliased_imports():
    import warnings

    memos: list[Any] = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    def assert_memo_replaced(_memos: list[Any]):
        assert len(_memos) == 2
        assert type(_memos[0]) is not dict
        assert type(_memos[1]) is dict

    from copium import deepcopy

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = deepcopy(A())

    assert len(w) == 1
    message = str(w[0].message)
    assert "deepcopy(A())" in message
    assert_memo_replaced(memos)
    memos.clear()

    import copium as c

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = c.deepcopy(A())

    assert len(w) == 1
    message = str(w[0].message)
    assert "c.deepcopy(A())" in message
    assert_memo_replaced(memos)
    memos.clear()


@pytest.mark.subprocess(environ=env())
def test_env_memo_fallback_warning_in_function():
    import warnings

    import copium

    memos: list[Any] = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    def do_copy(obj):
        return copium.deepcopy(obj)

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = do_copy(A())

    assert len(w) == 1
    message = str(w[0].message)
    assert "copium.deepcopy(obj)" in message
    assert "do_copy" in message
    assert len(memos) == 2


@pytest.mark.subprocess(environ=env())
def test_env_memo_fallback_warning_in_method():
    import warnings

    import copium

    memos: list[Any] = []

    class Target:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return Target()

    class Copier:
        def copy_it(self, obj):
            return copium.deepcopy(obj)

    copier = Copier()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = copier.copy_it(Target())

    assert len(w) == 1
    message = str(w[0].message)
    assert "copium.deepcopy(obj)" in message
    assert "copy_it" in message
    assert len(memos) == 2


@pytest.mark.subprocess(
    environ=env(COPIUM_NO_MEMO_FALLBACK_WARNING="TypeError: memo must be a dict")
)
def test_env_suppress_warning():
    import warnings

    import copium

    memos = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = copium.deepcopy(A())

    assert not w, f"Expected no warnings, got: {str(w[0].message) if w else ''}"
    assert len(memos) == 2
    assert type(memos[0]) is not dict
    assert type(memos[1]) is dict


@pytest.mark.subprocess(environ=env(COPIUM_NO_MEMO_FALLBACK_WARNING="TypeError: different error"))
def test_env_suppress_warning_non_matching():
    import warnings

    import copium

    class A:
        def __deepcopy__(self, memo):
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = copium.deepcopy(A())

    assert len(w) == 1


@pytest.mark.subprocess(environ=env(COPIUM_NO_MEMO_FALLBACK="1"))
def test_env_no_memo_fallback():
    import pytest

    import copium

    memos = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    with pytest.raises(TypeError, match="memo must be a dict"):
        _result = copium.deepcopy(A())

    assert len(memos) == 1
    assert type(memos[0]) is not dict


@pytest.mark.subprocess(environ=env(COPIUM_USE_DICT_MEMO="1"))
def test_env_use_dict_memo():
    import warnings

    import copium

    memos = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = copium.deepcopy(A())

    assert not w
    assert len(memos) == 1
    assert type(memos[0]) is dict


@pytest.mark.subprocess(environ=env(COPIUM_USE_DICT_MEMO="1"))
def test_env_maps_to_get_config():
    import copium

    cfg = copium.get_config()
    assert cfg["memo"] == "dict"


@pytest.mark.subprocess(environ=env(COPIUM_NO_MEMO_FALLBACK="1"))
def test_env_no_fallback_maps_to_get_config():
    import copium

    cfg = copium.get_config()
    assert cfg["on_incompatible"] == "raise"


@pytest.mark.subprocess(environ=env(COPIUM_NO_MEMO_FALLBACK_WARNING="TypeError: x"))
def test_env_suppress_maps_to_get_config():
    import copium

    cfg = copium.get_config()
    assert cfg["suppress_warnings"] == ("TypeError: x",)


@pytest.mark.subprocess(environ=env(COPIUM_USE_DICT_MEMO="1"))
def test_env_configure_overrides_env():
    """configure() overrides env-var defaults."""
    import copium

    assert copium.get_config()["memo"] == "dict"
    copium.configure(memo="native")
    assert copium.get_config()["memo"] == "native"


@pytest.mark.subprocess(environ=env(COPIUM_USE_DICT_MEMO="1"))
def test_env_configure_reset_reads_env():
    """configure() with no args re-reads env vars."""
    import copium

    copium.configure(memo="native")
    assert copium.get_config()["memo"] == "native"
    copium.configure()
    assert copium.get_config()["memo"] == "dict"


def copium_is_editable() -> bool:
    return Path(copium.__file__).parent.name == "src"


@pytest.mark.subprocess(environ=env(COPIUM_PATCH_ENABLE="1"))
@pytest.mark.xfail(
    copium_is_editable() and os.getenv("COPIUM_LOCAL_DEVELOPMENT"),
    reason=".pth files in editable installs are broken at the moment.",
    strict=True,
)
def test_env_patch_enabled():
    import copium.patch

    assert copium.patch.enabled()


@pytest.mark.subprocess(environ=env())
def test_env_patch_disabled():
    import copium.patch

    assert not copium.patch.enabled()


@pytest.mark.subprocess(environ=env())
def test_env_explicit_dict_memo_no_warning():
    import warnings

    import copium

    memos = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = copium.deepcopy(A(), {})

    assert not w
    assert len(memos) == 1
    assert type(memos[0]) is dict
