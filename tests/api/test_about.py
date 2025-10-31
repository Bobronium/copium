# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
from typing import Literal

import pytest

try:
    from typing import assert_type  # type: ignore[attr-defined,unused-ignore]
except ImportError:
    from typing_extensions import assert_type

import copium.__about__  # type: ignore[reportMissingModuleSource,unused-ignore]

FIRST_AUTHOR_EMAIL = Literal["hi@bobronium.me"]
FIRST_AUTHOR_NAME = Literal["Arseny Boykov (Bobronium)"]


@pytest.mark.typecheck
def test_about() -> None:
    assert_type(copium.__about__.__version__, str)
    assert_type(copium.__about__.__version_tuple__, copium.__about__.VersionInfo)
    assert_type(copium.__about__.__version_tuple__.major, int)
    assert_type(copium.__about__.__version_tuple__.minor, int)
    assert_type(copium.__about__.__version_tuple__.patch, int)
    assert_type(copium.__about__.__version_tuple__.prerelease, str | None)
    assert_type(copium.__about__.__version_tuple__.build, int | None)
    assert_type(copium.__about__.__version_tuple__.build_hash, str)
    assert_type(copium.__about__.__commit_id__, str | None)
    assert_type(
        copium.__about__.__authors__[0],
        copium.__about__.Author[FIRST_AUTHOR_NAME, FIRST_AUTHOR_EMAIL],
    )
    assert_type(copium.__about__.__authors__[0].name, FIRST_AUTHOR_NAME)
    assert_type(copium.__about__.__authors__[0].email, FIRST_AUTHOR_EMAIL)
