import collections
import copy as stdlib_copy
import sys
import weakref
from collections.abc import Generator
from contextlib import contextmanager
from types import MappingProxyType
from typing import Any

import pytest
from indifference import assert_equivalent_transformations

import copium
from datamodelzoo import CASES
from tests.conftest import CASE_PARAMS


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


def test_deepcopy_keepalive_internal_add(copy) -> None:
    """
    Note: this test is not part of Lib/test/test_copy.py.
    """
    x = []

    class A:
        ref: weakref.ref = None

        def __deepcopy__(self, memo):
            if A.ref is None:
                A.ref = weakref.ref(ref := A())
                memo.setdefault(id(memo), []).append(ref)
            else:
                assert A.ref(), "expected keepalive to keep track of A.ref"
            return self

    copied = copy.deepcopy([x, a := A(), b := A()])

    assert copied == [x, a, b]


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


memo_options = ["absent", "dict", "None", "mutable_mapping", "mapping", "invalid"]


@pytest.mark.parametrize("memo", memo_options, ids=[f"memo_{option}" for option in memo_options])
@pytest.mark.parametrize("case", CASE_PARAMS)
def test_duper_deepcopy_parity(case: Any, copy, memo) -> None:
    builtin_deepcopy_error = None



    def get_kwargs():
        if memo == "dict":
            kwargs = {"memo": {}}
        elif memo == "None":
            kwargs = {"memo": None}
        elif memo == "mapping":
            kwargs = {"memo": collections.UserDict()}
        elif memo == "mutable_mapping":
            kwargs = {"memo": MappingProxyType({})}  # expected to throw
        elif memo == "invalid":
            kwargs = {"memo": "not a memo"}
        else:
            kwargs = {}
        return kwargs

    try:
        baseline = stdlib_copy.deepcopy(case.obj, **get_kwargs())
    except Exception as e:
        baseline = None
        builtin_deepcopy_error = e

    candidate_name = f"{copy.deepcopy.__module__}.{copy.deepcopy.__name__}"
    try:
        candidate = copy.deepcopy(case.obj, **get_kwargs())
    except Exception as e:
        if builtin_deepcopy_error is None:
            raise AssertionError(
                f"{candidate_name} failed unexpectedly when copy.deepcopy didn't"
            ) from e
        assert type(e) is type(builtin_deepcopy_error), (
            f"{candidate_name} failed with different error"
        )
        assert e.args == builtin_deepcopy_error.args, (
            f"{candidate_name} failed with different error message"
        )
    else:
        if builtin_deepcopy_error is not None:
            raise AssertionError(
                f"{candidate_name} expected to produce {builtin_deepcopy_error!r} exception,"
                f" but didn't"
            ) from builtin_deepcopy_error

        assert_equivalent_transformations(
            case.obj,
            baseline,
            candidate,
        )


@pytest.mark.xfail(
    not getattr(sys, "_is_gil_enabled", lambda: True)(),
    reason="This test is flaky and sometimes fails on stdlib as well."
    " A better suite should be designed.",
)
@pytest.mark.xfail(reason="WIP")
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


def make_nested(depth):
    result = []
    for _ in range(depth):
        result = [result]
    return result


@contextmanager
def recursion_limit(depth: int) -> Generator[None]:
    current_limit = sys.getrecursionlimit()
    sys.setrecursionlimit(depth)
    try:
        yield
    finally:
        sys.setrecursionlimit(current_limit)


@pytest.mark.xfail(
    raises=RecursionError,
    reason="We won't guarantee larger than interpreter stack recursion, but it may happen.",
)
def test_recursion_error():
    above_interpreter_limit = make_nested(600)

    with recursion_limit(500):
        copium.deepcopy(above_interpreter_limit)


@pytest.mark.xfail(
    raises=RecursionError,
    reason="We won't guarantee larger than interpreter stack recursion, but it may happen.",
)
def test_recursion_limit_increase():
    baseline_limit = sys.getrecursionlimit()

    new_recursion_limit = baseline_limit + 10000
    at_interpreter_limit = make_nested(new_recursion_limit)
    with recursion_limit(new_recursion_limit):
        copium.deepcopy(at_interpreter_limit)


@pytest.mark.skip(reason="WIP")
def test_graceful_recursion_error():
    value = make_nested(999999)
    with pytest.raises(RecursionError):
        copium.deepcopy(value)  # without safeguards this can SIGSEGV


@pytest.mark.skip(reason="WIP")
def test_graceful_recursion_error_with_increased_limit():
    """
    We won't guarantee to match interpreter recursion limit, but will handle it gracefully.
    """
    too_large = 999999
    value = make_nested(too_large)
    with recursion_limit(too_large), pytest.raises(RecursionError):
        copium.deepcopy(value)  # without safeguards this can SIGSEGV


def test_memo_reused():
    class Observative:
        observations = set()  # noqa: RUF012

        def __deepcopy__(self, memo):
            self.observations.add(id(memo))

    ref_thieves = []
    for _i in range(1000):
        obj = [[], a := Observative(), a, "123", {}]
        ref_thieves.append({})
        copied = copium.deepcopy(obj)
        ref_thieves.append({})
        assert copied == [[], None, None, "123", {}]
        assert copied[0] is not obj[0]
        assert copied[-1] is not obj[-1]

    assert len(Observative.observations) == 1

    for _i in range(1000):
        obj = [[], a := Observative(), a, "123", {}]
        ref_thieves.append({})
        stdlib_copy.deepcopy(obj)
        ref_thieves.append({})

    # might become flaky at some point, but serves the purpose now
    assert len(Observative.observations) == 1001


def test_memo_reference_stolen():
    class Nostalgic:
        memories = {}  # noqa: RUF012

        def __deepcopy__(self, memo):
            self.memories[id(memo)] = memo

    for _i in range(100):
        obj = [[], a := Nostalgic(), a, "123", {}]

        assert_equivalent_transformations(obj, stdlib_copy.deepcopy(obj), copium.deepcopy(obj))

    assert len(Nostalgic.memories) == 200


def test_memo_reference_passthrough():
    class Chaotic:
        observations = set()  # noqa: RUF012

        def __init__(self, foo):
            self.foo = foo

        def __deepcopy__(self, memo):
            if type(memo) is not dict:
                self.observations.add(id(memo))
            Chaotic(copium.deepcopy(self.foo, memo))

    for _i in range(100):
        obj = [[], a := Chaotic({1, 2, 3}), a, "123", {}]
        assert_equivalent_transformations(obj, stdlib_copy.deepcopy(obj), copium.deepcopy(obj))

    assert len(Chaotic.observations) == 1


def test_no_extra_refs_post_deepcopy(copy):
    original = [object(), object(), object()]
    original_refcounts_before_copying = [sys.getrefcount(obj) for obj in original]

    copied = copy.deepcopy(original)  # hold refs
    original_refcounts_after_copying = [sys.getrefcount(obj) for obj in original]
    copied_refcounts = [sys.getrefcount(obj) for obj in copied]

    assert original_refcounts_before_copying == original_refcounts_after_copying
    assert copied_refcounts == original_refcounts_before_copying


def test_holding_extra_refs_post_deepcopy(copy):
    memories = []

    class Sneaky:
        def __deepcopy__(self, memo):
            memories.append(memo)
            return Sneaky()

    original = [Sneaky(), object(), object()]
    original_refcounts_before_copying = [sys.getrefcount(obj) for obj in original]

    copied = copy.deepcopy(original)  # hold refs
    original_refcounts_after_copying = [sys.getrefcount(obj) for obj in original]
    copied_refcounts = [sys.getrefcount(obj) for obj in copied]

    assert sum(original_refcounts_after_copying) - sum(original_refcounts_before_copying) == 3
    assert copied_refcounts == original_refcounts_after_copying

    copied_again = copy.deepcopy(original)  # hold refs
    copied_refcounts = [sys.getrefcount(obj) for obj in copied_again]
    original_refcounts_after_copying = [sys.getrefcount(obj) for obj in original]

    assert sum(original_refcounts_after_copying) - sum(original_refcounts_before_copying) == 6
    assert sum(original_refcounts_after_copying) - sum(copied_refcounts) == 3
