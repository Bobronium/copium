# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT

import pytest

try:
    from typing import assert_type  # type: ignore[attr-defined,unused-ignore]
except ImportError:
    from typing_extensions import assert_type

import copium.extra  # type: ignore[reportMissingModuleSource,unused-ignore]
from tests.api import XT
from tests.api import X


@pytest.mark.typecheck
def test_extra() -> None:
    assert_type(copium.extra.replicate(X, 1), list[XT])
    assert_type(copium.extra.repeatcall(lambda: X, 1), list[XT])
