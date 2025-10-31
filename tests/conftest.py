# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
import copy as stdlib_copy
import sys

import pytest

import copium
from datamodelzoo import CASES


def pytest_configure(config):
    config.option.snapshot_dirname = f".expected/{sys.version_info.major}.{sys.version_info.minor}/"
    config.option.snapshot_patch_pycharm_diff = True


CASE_PARAMS = [pytest.param(case, id=case.name) for case in CASES]


class CopyModule:  # just for typing
    error = Error = copium.Error
    copy = staticmethod(copium.copy)
    deepcopy = staticmethod(copium.deepcopy)
    if sys.version_info >= (3, 13):
        replace = staticmethod(copium.replace)


@pytest.fixture(
    params=[
        pytest.param(stdlib_copy, id="stdlib"),
        pytest.param(copium, id="copium"),
        # sanity check
    ]
)
def copy(request) -> "CopyModule":
    return request.param
