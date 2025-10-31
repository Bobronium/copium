# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
import re
import sys
from types import MappingProxyType
from typing import TYPE_CHECKING
from typing import NamedTuple

import pytest

try:
    from typing import assert_never  # type: ignore[attr-defined,unused-ignore]
    from typing import assert_type  # type: ignore[attr-defined,unused-ignore]
except ImportError:
    from typing_extensions import assert_never
    from typing_extensions import assert_type

import copium
from tests.api import XT
from tests.api import X


@pytest.mark.typecheck
def test_copy() -> None:
    assert_type(copium.copy(X), XT)

    with pytest.raises(AssertionError):
        assert_type(copium.deepcopy(1), XT)  # type: ignore[misc,type-assertion-failure,assert-type,unused-ignore]

    assert_type(copium.deepcopy(X), XT)
    assert_type(copium.deepcopy(X, {}), XT)
    assert_type(copium.deepcopy(X, memo={}), XT)
    assert_type(copium.deepcopy(X, memo=None), XT)
    assert_type(copium.deepcopy(x=X, memo={}), XT)

    if sys.version_info >= (3, 13):

        class A(NamedTuple):
            a: int

        assert_type(copium.replace(A(1), a=2), A)


@pytest.mark.typecheck
def test_copy_errors() -> None:
    if not TYPE_CHECKING:
        with pytest.raises(TypeError, match=r"missing 1 required positional argument: 'x'"):
            copium.deepcopy()

        with pytest.raises(
            TypeError, match=r"takes from 1 to 2 positional arguments but 3 were given"
        ):
            copium.deepcopy([1, 2, 3], {}, "extra")
        with pytest.raises(
            TypeError, match=r"takes from 1 to 2 positional arguments but 4 were given"
        ):
            copium.deepcopy([1, 2, 3], {}, "extra1", "extra2")

        with pytest.raises(TypeError, match=r"got multiple values for argument 'memo'"):
            assert_never(copium.deepcopy([1, 2, 3], {}, memo={}))
        with pytest.raises(TypeError, match=r"got an unexpected keyword argument 'foo'"):
            assert_never(copium.deepcopy([1, 2, 3], foo={}))
        with pytest.raises(TypeError, match=r"got an unexpected keyword argument 'bar'"):
            assert_never(copium.deepcopy([1, 2, 3], bar={}))

        class CustomClass:
            pass

        with pytest.raises(AttributeError, match=r"'list' object has no attribute 'get'"):
            assert_never(copium.deepcopy([1, 2, 3], memo=[]))

        with pytest.raises(AttributeError, match=r"'str' object has no attribute 'get'"):
            assert_never(copium.deepcopy([1, 2, 3], memo="string"))

        with pytest.raises(AttributeError, match=r"'int' object has no attribute 'get'"):
            assert_never(copium.deepcopy([1, 2, 3], memo=42))

        with pytest.raises(AttributeError, match=r"'CustomClass' object has no attribute 'get'"):
            assert_never(copium.deepcopy([1, 2, 3], memo=CustomClass()))

        with pytest.raises(
            TypeError, match=r"'mappingproxy' object does not support item assignment"
        ):
            assert_never(copium.deepcopy([1, 2, 3], memo=MappingProxyType({})))

        # copy: missing/too many arguments
        with pytest.raises(
            TypeError,
            match=re.escape(r"copium.copy() takes exactly one argument (0 given)"),
        ):
            assert_never(copium.copy())
        with pytest.raises(
            TypeError, match=re.escape(r"copium.copy() takes exactly one argument (2 given)")
        ):
            assert_never(copium.copy([1, 2, 3], "extra"))

        if sys.version_info >= (3, 13):
            # replace: missing/too many positional arguments
            with pytest.raises(TypeError, match=r"missing 1 required positional argument: 'obj'"):
                assert_never(copium.replace())
            from types import SimpleNamespace

            with pytest.raises(TypeError, match=r"takes 1 positional argument but 2 were given"):
                assert_never(copium.replace(SimpleNamespace(x=1), "extra"))

            with pytest.raises(TypeError, match=r"takes 1 positional argument but 3 were given"):
                assert_never(copium.replace(SimpleNamespace(x=1), "a", "b"))
