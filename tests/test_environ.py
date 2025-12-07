import os
from pathlib import Path

import pytest

import copium


def env(**kwargs):
    base = {
        "COPIUM_NO_MEMO_FALLBACK_WARNING": None,
        "COPIUM_NO_MEMO_FALLBACK": None,
        "COPIUM_USE_DICT_MEMO": None,
        "COPIUM_PATCH_DEEPCOPY": None,
    }
    base.update(kwargs)
    return base


@pytest.mark.subprocess(environ=env())
def test_memo_fallback_warning():
    import warnings
    from typing import Any

    import copium

    memos: list[Any] = []

    class A:
        def __deepcopy__(self, memo):
            memos.append(memo)
            if type(memo) is not dict:
                raise TypeError("memo must be a dict")
            return A()

    def assert_warning_message_matches(
        caught_warnings,
        deepcopy_expr: str,
        call_site_line: str,
    ):
        assert len(caught_warnings) == 1, f"Expected 1 warning, got {len(caught_warnings)}"
        message = str(caught_warnings[0].message)
        assert issubclass(caught_warnings[0].category, UserWarning)

        assert f"'copium.memo' was rejected inside '{__name__}.A.__deepcopy__'" in message, message
        assert 'raise TypeError("memo must be a dict")' in message, message
        assert (
            f"Per Python docs, '{__name__}.A.__deepcopy__' should treat memo as an opaque object."
            in message
        ), message

        assert call_site_line in message, f"{call_site_line!r} not found in {message}"

        deepcopy_expr_with_memo = deepcopy_expr[:-1].rstrip(",") + ", {})"
        assert f"change {deepcopy_expr} to {deepcopy_expr_with_memo}" in message, (
            f"Expected 'change {deepcopy_expr} to {deepcopy_expr_with_memo}' in message:\n{message}"
        )

        assert "export COPIUM_NO_MEMO_FALLBACK_WARNING='TypeError: memo must be a dict'" in message
        assert f"'{deepcopy_expr}' stays slow to deepcopy" in message
        assert f"'{deepcopy_expr}' raises the error above" in message

    def assert_memo_replaced(_memos: list[Any]):
        assert len(_memos) == 2, f"Expected 2 memo calls, got {len(_memos)}"
        on_first_call, on_second_call = _memos
        assert type(on_first_call) is not dict, "First call should use copium.memo"
        assert type(on_second_call) is dict, "Second call should use dict (fallback)"
        _memos.clear()

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        __result = copium.deepcopy(A())

    assert_warning_message_matches(
        w,
        deepcopy_expr="copium.deepcopy(A())",
        call_site_line="__result = copium.deepcopy(A())",
    )
    assert_memo_replaced(memos)
    memos.clear()

    def identity(x):
        return x

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = identity(copium.deepcopy(A()))

    assert_warning_message_matches(
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

    assert_warning_message_matches(
        w,
        deepcopy_expr="deepcopy(A())",
        call_site_line="_result = copium.deepcopy(",
    )
    assert_memo_replaced(memos)

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        assert copium.deepcopy(A()) is not None

    assert_warning_message_matches(
        w,
        deepcopy_expr="copium.deepcopy(A())",
        call_site_line="assert copium.deepcopy(A()) is not None",
    )
    assert_memo_replaced(memos)

    # Case 6: Trailing comma (edgy but valid Python)
    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        # fmt: off
        _result = copium.deepcopy(A(),)  # noqa: COM819
        # fmt: on

    assert_warning_message_matches(
        w, "copium.deepcopy(A(),)", "_result = copium.deepcopy(A(),)  # noqa: COM819"
    )
    assert_memo_replaced(memos)


@pytest.mark.subprocess(environ=env())
def test_memo_fallback_warning_aliased_imports():
    """Test with various import styles"""
    import warnings
    from typing import Any

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

    # Case 1: from copium import deepcopy
    from copium import deepcopy

    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter("always")
        _result = deepcopy(A())

    assert len(w) == 1
    message = str(w[0].message)
    assert "deepcopy(A())" in message
    assert_memo_replaced(memos)
    memos.clear()

    # Case 2: import copium as c
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
def test_memo_fallback_warning_in_function():
    """Test when deepcopy is called from inside a user function"""
    import warnings
    from typing import Any

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
    # Should show the call site inside do_copy
    assert "copium.deepcopy(obj)" in message
    assert "do_copy" in message
    assert len(memos) == 2


@pytest.mark.subprocess(environ=env())
def test_memo_fallback_warning_in_method():
    """Test when deepcopy is called from inside a class method"""
    import warnings
    from typing import Any

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
def test_no_memo_fallback_warning():
    """COPIUM_NO_MEMO_FALLBACK_WARNING suppresses matching warnings"""
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
def test_no_memo_fallback_warning_non_matching():
    """COPIUM_NO_MEMO_FALLBACK_WARNING only suppresses matching errors"""
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
def test_no_memo_fallback():
    """COPIUM_NO_MEMO_FALLBACK=1 disables fallback entirely"""
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
def test_use_dict_memo():
    """COPIUM_USE_DICT_MEMO=1 uses dict memo from the start"""
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


def copium_is_editable() -> bool:
    return Path(copium.__file__).parent.name == "src"


@pytest.mark.subprocess(environ=env(COPIUM_PATCH_DEEPCOPY="1"))
@pytest.mark.xfail(
    copium_is_editable() and os.getenv("COPIUM_LOCAL_DEVELOPMENT"),
    reason=".pth files in editable installs are broken at the moment.",
    strict=True,
)
def test_patch_deepcopy_enabled():
    """COPIUM_PATCH_DEEPCOPY=1 enables patching"""
    import copium.patch

    assert copium.patch.enabled()


@pytest.mark.subprocess(environ=env())
def test_patch_deepcopy_disabled():
    """Patching is disabled by default"""
    import copium.patch

    assert not copium.patch.enabled()


@pytest.mark.subprocess(environ=env())
def test_explicit_dict_memo_no_warning():
    """Providing explicit dict memo should not trigger fallback warning"""
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
