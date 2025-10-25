# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT

import pytest
from typing_extensions import assert_type

import copium.extra  # type: ignore[reportMissingModuleSource,unused-ignore]
from tests.typehints.conftest import XT
from tests.typehints.conftest import X


@pytest.mark.typecheck
def test_extra() -> None:
    assert_type(copium.extra.replicate(X, 1), list[XT])
    assert_type(copium.extra.repeatcall(lambda: X, 1), list[XT])
