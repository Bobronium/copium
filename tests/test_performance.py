from typing import Any

import pytest
from datamodelzoo import CASES


@pytest.mark.parametrize(
    "case",
    (pytest.param(case, id=case.name) for case in CASES if "raises" not in case.name),
)
@pytest.mark.benchmark
def test_individual_cases(case: Any, copy) -> None:
    copy.deepcopy(case.obj)
