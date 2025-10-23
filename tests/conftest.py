# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
import copy as stdlib_copy
import sys
from functools import lru_cache

import _pytest._code.source
import pytest
from datamodelzoo import CASES

import copyc


def pytest_configure(config):
    config.option.snapshot_dirname = f".expected/{sys.version_info.major}.{sys.version_info.minor}/"
    config.option.snapshot_patch_pycharm_diff = True


CASES = [pytest.param(case, id=case.name) for case in CASES]


class CopyModule:  # just for typing
    error = Error = stdlib_copy.Error
    copy = staticmethod(stdlib_copy.copy)
    deepcopy = staticmethod(copyc.deepcopy)


@pytest.fixture(
    params=[
        pytest.param(copyc, id="copyc"),
        # sanity check
        pytest.param(stdlib_copy, id="stdlib"),
    ]
)
def copy(request) -> "CopyModule":
    return request.param
