import platform
import random
import sys
from typing import Any

import pytest
from datamodelzoo import CASES
from datamodelzoo import Case

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

python_version = ".".join(map(str, sys.version_info[:2]))
if not getattr(sys, "_is_gil_enabled", lambda: True)():
    python_version += "t"
python_version += f"-{platform.machine()}"

if python_version == "3.13-x86_64":
    # backwards compatibility with previous benchmarks runs

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

else:

    @pytest.mark.parametrize(
        "case",
        (pytest.param(case, id=case.name) for case in COMBINED_CASES),
    )
    @pytest.mark.parametrize("_python", [python_version])
    def test_combined_cases(case: Any, copy, benchmark, _python) -> None:
        benchmark(copy.deepcopy, case.obj)

    @pytest.mark.parametrize(
        "case",
        (pytest.param(case, id=case.name) for case in BASE_CASES),
    )
    @pytest.mark.parametrize("_python", [python_version])
    def test_individual_cases(case: Any, copy, benchmark, _python) -> None:
        benchmark(copy.deepcopy, case.obj)
