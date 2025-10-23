# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
import sys
from typing import NamedTuple

import pytest
from typing_extensions import assert_type

import copyc
from tests.typehints.conftest import XT
from tests.typehints.conftest import X


@pytest.mark.typecheck
def test_copy() -> None:
    assert_type(copyc.copy(X), XT)

    with pytest.raises(AssertionError):
        assert_type(copyc.deepcopy(1), XT)  # type: ignore[misc,type-assertion-failure,assert-type,unused-ignore]

    assert_type(copyc.deepcopy(X, {}), XT)

    if sys.version_info >= (3, 13):

        class A(NamedTuple):
            a: int

        assert_type(copyc.replace(A(1), a=2), A)
