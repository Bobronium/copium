# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT

import pytest
from typing_extensions import assert_type

import copyc.extra  # type: ignore[reportMissingModuleSource]
from tests.typehints.conftest import XT
from tests.typehints.conftest import X


@pytest.mark.typecheck
def test_extra() -> None:
    assert_type(copyc.extra.replicate(X, 1), list[XT])
    assert_type(copyc.extra.repeatcall(lambda: X, 1), list[XT])
