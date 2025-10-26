import random
from typing import Any

import pytest
from datamodelzoo import CASES, Case


BASE_CASES = [case for case in CASES if "raises" not in case.name and "thirdparty" not in case.name]

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


@pytest.mark.parametrize(
    "case",
    (pytest.param(case, id=case.name) for case in COMBINED_CASES),
)
def test_combined_cases(case: Any, copy, benchmark) -> None:
    benchmark(copy.deepcopy, case.obj)


@pytest.mark.parametrize(
    "case",
    (pytest.param(case, id=case.name) for case in BASE_CASES),
)
def test_individual_cases(case: Any, copy, benchmark) -> None:
    benchmark(copy.deepcopy, case.obj)
