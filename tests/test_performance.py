# SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
#
# SPDX-License-Identifier: MIT

import copy as stdlib_copy
import platform
import random
import sys
from itertools import chain
from typing import Any

import pytest

import copium
import copium.patch
from datamodelzoo import CASES
from datamodelzoo import Case

BASE_CASES = [
    case
    for case in CASES
    if "raises" not in case.name and "thirdparty" not in case.name and "guard" not in case.name
]
GUARD_CASES = [case for case in CASES if "guard" in case.name]

random.seed(1)

COMBINED_CASES = [
    Case(
        "all",
        factory=lambda: (c := [case.obj for case in BASE_CASES] * 1000, random.shuffle(c), c)[-1],
    ),
    Case(
        "cpython:91610",
        factory=lambda: [case.obj for case in BASE_CASES if "91610" in case.name],
    ),
    Case(
        "diverse_atomic",
        factory=lambda: [case.obj for case in BASE_CASES if "atom:" in case.name] * 1000,
    ),
    Case(
        "all_proto",
        factory=lambda: [case.obj for case in BASE_CASES if "proto:" in case.name] * 1000,
    ),
    Case(
        "all_reflexive",
        factory=lambda: [case.obj for case in BASE_CASES if "reflexive" in case.name] * 10,
    ),
    Case(
        "all_empty",
        factory=lambda: [case.obj for case in BASE_CASES if "empty" in case.name] * 100,
    ),
    Case(
        "all_stdlib",
        factory=lambda: [case.obj for case in BASE_CASES if "stdlib" in case.name] * 1000,
    ),
]

python_version = ".".join(map(str, sys.version_info[:2]))
if not getattr(sys, "_is_gil_enabled", lambda: True)():
    python_version += "t"
python_version += f"-{platform.machine()}"

PYTHON_VERSION_PARAM = pytest.mark.parametrize("_python", [python_version])

COMBINED_CASES_PARAMS = pytest.mark.parametrize(
    "case",
    [pytest.param(case, id=case.name) for case in COMBINED_CASES],
)

BASE_CASES_PARAMS = pytest.mark.parametrize(
    "case",
    (pytest.param(case, id=case.name) for case in chain(BASE_CASES, GUARD_CASES)),
)


@BASE_CASES_PARAMS
@PYTHON_VERSION_PARAM
def test_individual_cases_warmup(case: Any, copy, _python, benchmark) -> None:
    copy.deepcopy(case.obj)


@COMBINED_CASES_PARAMS
@PYTHON_VERSION_PARAM
def test_combined_cases_warmup(case: Any, copy, _python, benchmark) -> None:
    copy.deepcopy(case.obj)


# Initially tests were only running on 3.13 x86_64
if python_version == "3.13-x86_64":
    # backwards compatibility with previous benchmarks runs

    @BASE_CASES_PARAMS
    def test_individual_cases(case: Any, copy, benchmark) -> None:
        benchmark(copy.deepcopy, case.obj)

    @COMBINED_CASES_PARAMS
    def test_combined_cases(case: Any, copy, benchmark) -> None:
        benchmark(copy.deepcopy, case.obj)

else:
    assert sys.version_info >= (3, 14) or not "--codspeed" in sys.argv, (
        "This block assumed to have newer versions only."
    )

    @BASE_CASES_PARAMS
    @PYTHON_VERSION_PARAM
    def test_individual_cases(case: Any, copy, benchmark, _python) -> None:
        benchmark(copy.deepcopy, case.obj)

    @COMBINED_CASES_PARAMS
    @PYTHON_VERSION_PARAM
    def test_combined_cases(case: Any, copy, benchmark, _python) -> None:
        benchmark(copy.deepcopy, case.obj)

    @COMBINED_CASES_PARAMS
    @PYTHON_VERSION_PARAM
    def test_combined_cases_copium_dict_memo(case: Any, benchmark, _python) -> None:
        benchmark(copium.deepcopy, case.obj, {})

    @COMBINED_CASES_PARAMS
    @PYTHON_VERSION_PARAM
    def test_combined_cases_stdlib_patched(
        case: Any, benchmark, _python, copium_patch_enabled
    ) -> None:
        benchmark(stdlib_copy.deepcopy, case.obj)
