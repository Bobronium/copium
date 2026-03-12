# SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
import re
from typing import TYPE_CHECKING
from typing import Literal
from typing import TypeAlias
from typing import TypedDict

import pytest

try:
    from typing import assert_never  # type: ignore[attr-defined,unused-ignore]
    from typing import assert_type  # type: ignore[attr-defined,unused-ignore]
except ImportError:
    from typing_extensions import assert_never
    from typing_extensions import assert_type

import copium

if TYPE_CHECKING:
    from copium.config import _CopiumConfig

    NoneType: TypeAlias = None
else:
    NoneType = type(None)

    class _CopiumConfig(TypedDict, total=True):
        memo: Literal["native", "dict"]
        on_incompatible: Literal["warn", "raise", "silent"]
        suppress_warnings: tuple[str, ...]


@pytest.mark.typecheck
def test_config() -> None:
    assert_type(copium.config.apply(), NoneType)
    assert_type(copium.config.apply(memo="dict"), NoneType)
    assert_type(copium.config.apply(memo="native"), NoneType)
    assert_type(copium.config.apply(memo="native", on_incompatible="warn"), NoneType)
    assert_type(
        copium.config.apply(memo="native", on_incompatible="warn", suppress_warnings=["TypeError"]),
        NoneType,
    )
    with pytest.raises(
        TypeError,
        match=re.escape(
            "when `memo='dict'`, `on_incompatible` and `suppress_warnings` are ambiguous:"
            " remove them or use `memo='native'`"
        ),
    ):
        assert_never(copium.config.apply(memo="dict", on_incompatible="warn"))  # type: ignore[misc,type-assertion-failure,call-overload,unused-ignore]

    assert_type(copium.config.get(), _CopiumConfig)
