import copy as stdlib_copy
import sys
from typing import Any

import pytest
from indifference import assert_equivalent_transformations

import copium
from tests.conftest import CASES


@pytest.mark.xfail(
    reason="copium will not expose keepalive list, unless memo was supplied by the user",
    raises=KeyError,
)
def test_deepcopy_keepalive_internal(copy) -> None:
    """
    Note: this test is not part of Lib/test/test_copy.py.
    """
    x = []

    class A:
        def __deepcopy__(self, memo):
            assert memo[id(memo)][0] is x
            return self

    copied = copy.deepcopy([x, a := A()])

    assert copied == [x, a]


def test_deepcopy_memo_dict_keepalive_internal(copy) -> None:
    """
    Note: this test is not part of Lib/test/test_copy.py.
    """
    x = []

    class A:
        def __deepcopy__(self, memo):
            assert memo[id(memo)][0] is x
            return self

    copied = copy.deepcopy([x, a := A()], {})

    assert copied == [x, a]


def test_mutable_keys(copy):
    from datamodelzoo.constructed import MutableKey

    original_key = MutableKey()
    original = {MutableKey("copied"): 420, original_key: 42}
    copied = copy.deepcopy(original)

    assert copied[MutableKey("copied")] == 42, "deepcopy computed wrong hash for copied key"
    assert original_key not in copied


@pytest.mark.parametrize("case", CASES)
def test_duper_deepcopy_parity(case: Any, copy) -> None:
    deepcopy_failed = False
    try:
        baseline = stdlib_copy.deepcopy(case.obj)
    except Exception as e:
        baseline = e
        deepcopy_failed = True

    try:
        candidate = copy.deepcopy(case.obj)
    except Exception as e:
        if not deepcopy_failed:
            raise AssertionError(
                f"{copy.deepcopy} failed unexpectedly when {stdlib_copy.deepcopy} didn't"
            ) from e
        assert type(e) is type(baseline), "copium failed with different error"
        assert e.args == baseline.args, "copium failed with different error message"
    else:
        assert_equivalent_transformations(
            case.obj,
            baseline,
            candidate,
        )


def test_duper_deepcopy_parity_threaded_mutating(copy) -> None:
    from concurrent.futures import ALL_COMPLETED
    from concurrent.futures import ThreadPoolExecutor
    from concurrent.futures import wait

    from datamodelzoo.constructed import DeepcopyRuntimeError

    threads = 8
    repeats = 5

    value: dict[Any, Any] = {}
    value["trigger"] = DeepcopyRuntimeError(value)

    def assert_runtime_error():
        try:
            copy.deepcopy(value)
        except RuntimeError:
            return True
        return False

    with ThreadPoolExecutor(max_workers=threads) as pool:
        total_runs = threads * repeats
        futures = [pool.submit(assert_runtime_error) for _ in range(total_runs)]

        done, not_done = wait(futures, return_when=ALL_COMPLETED)
        assert not not_done
        correct_runs = sum(1 for f in done if f.result())
        assert correct_runs == total_runs


def test_recursion_error():
    def make_nested(depth):
        result = {}
        for _ in range(depth):
            result = {"child": result}
        return result

    recursion_limit = sys.getrecursionlimit()

    at_interpreter_limit = make_nested(256)

    try:
        sys.setrecursionlimit(256)
        with pytest.raises(RecursionError):
            stdlib_copy.deepcopy(at_interpreter_limit)

        copied = copium.deepcopy(at_interpreter_limit)
    finally:
        sys.setrecursionlimit(recursion_limit)

    assert copied == at_interpreter_limit, "Unexpectedly affected by interpreter recursion limit"

    value = make_nested(99999)
    with pytest.raises(RecursionError):
        copium.deepcopy(value)  # without safeguards this can SIGSEGV
