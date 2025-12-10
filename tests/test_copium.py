# SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
#
# SPDX-License-Identifier: MIT

import collections
import copy as stdlib_copy
import gc
import random
import sys
import threading
import time
import weakref
from collections.abc import Callable
from collections.abc import Generator
from collections.abc import MutableMapping
from contextlib import contextmanager
from types import MappingProxyType
from typing import Any
from typing import Literal

import pytest
from indifference import assert_equivalent_transformations

import copium
from datamodelzoo import Case
from tests.conftest import CASE_PARAMS
from tests.conftest import EVIL_CASE_PARAMS
from tests.conftest import CopyModule

ValidMemoOptions = Literal["absent", "dict", "None", "mutable_mapping"]
AllMemoOption = Literal["absent", "dict", "None", "mutable_mapping", "mappingproxy", "invalid"]
AbsentMemoOption = Literal["absent", "None"]


def memo_params_from(literal: Any) -> Any:
    return [pytest.param(option, id=f"memo_{option}") for option in literal.__args__]


ALL_MEMO_PARAMS = memo_params_from(AllMemoOption)
VALID_MEMO_PARAMS = memo_params_from(ValidMemoOptions)
ABSENT_MEMO_PARAMS = memo_params_from(AbsentMemoOption)


def memo_kwargs(memo: AllMemoOption) -> dict[str, Any]:
    if memo == "dict":
        kwargs = {"memo": {}}
    elif memo == "None":
        kwargs = {"memo": None}
    elif memo == "memo_mappingproxy":
        kwargs = {"memo": MappingProxyType({})}  # expected to throw
    elif memo == "mutable_mapping":
        kwargs = {"memo": collections.UserDict()}
    elif memo == "invalid":
        kwargs = {"memo": "not a memo"}
    else:
        kwargs = {}
    return kwargs


class ContainerThatUsesDifferentCopy:
    def __init__(self, data, copier):
        self.data = data
        self.copier = copier

    def __deepcopy__(self, memo: MutableMapping):
        data = self.copier(self.data, memo)
        return ContainerThatUsesDifferentCopy(data, self.copier)


EXPECTED_ERROR_DIVERGENCES = {
    repr(TypeError("EvilReduceArgs() takes no arguments")): repr(
        TypeError("second item of the tuple returned by __reduce__ must be a tuple, not str")
    ),
    repr(TypeError("'int' object is not callable")): repr(
        TypeError("first item of the tuple returned by __reduce__ must be callable, not int")
    ),
    repr(TypeError("_reconstruct() missing 1 required positional argument: 'args'")): repr(
        TypeError("tuple returned by __reduce__ must contain 2 through 5 elements")
    ),
    repr(AttributeError("'EvilStateSlotsMapping' object has no attribute 'items'")): repr(
        TypeError("slot state is not a dictionary")
    ),
    repr(ValueError("not enough values to unpack (expected 2, got 1)")): repr(
        ValueError("dictiter must yield (key, value) pairs")
    ),
}


@pytest.mark.parametrize("memo_option", VALID_MEMO_PARAMS)
def test_deepcopy_memo_implementation_details(copy, memo_option) -> None:
    """
    This test is here mostly to document current behavior, not necessarily to enforce it.

    Python docs explicitly say to treat memo as an opaque object:
    https://docs.python.org/3.14/library/copy.html#object.__deepcopy__
    """

    class A:
        def __deepcopy__(self, memo: MutableMapping):
            self.memo = memo
            return self

    container = [x := [], a := A()]
    copied = copy.deepcopy(container, **memo_kwargs(memo_option))

    assert copied == [x, a]
    assert copied[1] is a

    memo: MutableMapping = a.memo

    assert memo[id(x)] is copied[0]
    assert memo[id(container)] is copied

    assert id(a) not in memo

    assert len(memo) == 3

    # keepalive is an implementation detail
    # and I haven't found any direct usage of it outside of Lib/copy.py.
    # Still, copium strives to be interchangeable, so it's good to document the behavior.
    keepalive = memo[id(memo)]
    try:
        # stdlib saves objects to keepalive only after they are deep copied
        assert keepalive[0] is x
    except AssertionError:
        # copium saves objects to keepalive as soon as they memoized
        assert keepalive[0] is container
        assert keepalive[1] is x
    else:
        assert keepalive[1] is container

    assert len(keepalive) == 2

    assert id(memo) in memo
    assert id(memo) in memo
    assert keepalive in memo.values()

    if memo_option in {"absent", "None"} and copy is copium:
        assert repr(memo) == f"memo({dict(memo)})"


@pytest.mark.parametrize("memo_option", VALID_MEMO_PARAMS)
def test_deepcopy_keepalive_internal_add(copy, memo_option) -> None:
    """
    Note: this test is not part of Lib/test/test_copy.py.
    """

    class Referenced: ...

    class A:
        weak_ref: weakref.ref = None

        def __deepcopy__(self, memo):
            """
            This will be called two times:
            1. create new object and store weakref to it on class body
            2. check that weak_ref is still pointing to the object
            """
            if A.weak_ref is None:
                A.weak_ref = weakref.ref(strong_ref := Referenced())
                memo.setdefault(id(memo), []).append(strong_ref)
            else:
                assert A.weak_ref(), "expected keepalive to keep track of A.ref"
            return self

    copied = copy.deepcopy([x := [], a := A(), b := A()], **memo_kwargs(memo_option))

    assert copied == [x, a, b]


def test_mutable_keys(copy):
    from datamodelzoo.constructed import MutableKey

    original_key = MutableKey()
    original = {MutableKey("copied"): 420, original_key: 42}
    copied = copy.deepcopy(original)

    assert copied[MutableKey("copied")] == 42, "deepcopy computed wrong hash for copied key"
    assert original_key not in copied


@pytest.mark.filterwarnings(r"ignore:\s+Seems like 'copium.memo' was rejected")
@pytest.mark.filterwarnings("error")
@pytest.mark.parametrize("memo", ALL_MEMO_PARAMS)
@pytest.mark.parametrize("case", CASE_PARAMS + EVIL_CASE_PARAMS)
def test_copium_deepcopy_parity_with_stdlib(case: Case, memo: AllMemoOption) -> None:
    """
    Make sure that copium produces the same behavior as stdlib.
    """
    assert_parity_with_stdlib(
        case,
        copium,
        memo,
        lambda: case.obj,
    )


@pytest.mark.filterwarnings(r"ignore:\s+Seems like 'copium.memo' was rejected")
@pytest.mark.filterwarnings("error")
@pytest.mark.parametrize("memo", ABSENT_MEMO_PARAMS)
@pytest.mark.parametrize("case", CASE_PARAMS + EVIL_CASE_PARAMS)
@pytest.mark.parametrize(
    "copy,nested_copy",
    [
        pytest.param(copium, stdlib_copy, id="copium-stdlib-nested"),
        pytest.param(copium, copium, id="copium-copium-nested"),
        pytest.param(stdlib_copy, copium, id="stdlib-copium-nested"),
    ],
)
def test_copium_deepcopy_compatibility_with_stdlib(
    case: Case,
    memo: AllMemoOption,
    copy: CopyModule,
    nested_copy,
) -> None:
    """
    Make sure that copium can be used interchangeably with deepcopy when using copium.memo.
    """
    assert_parity_with_stdlib(
        case,
        copy,
        memo,
        lambda: ContainerThatUsesDifferentCopy(case.obj, nested_copy.deepcopy),
    )


def assert_parity_with_stdlib(
    case: Case,
    copy: CopyModule,
    memo: Literal["absent", "dict", "None", "mutable_mapping", "mappingproxy", "invalid"],
    get_obj: Callable[[], Any],
):
    candidate_name = f"{copy.deepcopy.__module__}.{copy.deepcopy.__name__}"

    try:
        baseline_value = stdlib_copy.deepcopy(get_obj(), **memo_kwargs(memo))
    except Exception as baseline_error:
        try:
            candidate_value = copy.deepcopy(get_obj(), **memo_kwargs(memo))
        except Exception as candidate_error:
            if "evil" in case.name:
                # relax 1:1 error requirement for cases that intentionally break protocol
                # still, good to track how we diverge from stdlib exactly
                expected_errors = {baseline_error_repr := repr(baseline_error)}
                if baseline_error_repr in EXPECTED_ERROR_DIVERGENCES:
                    expected_errors.add(EXPECTED_ERROR_DIVERGENCES[baseline_error_repr])

                assert repr(candidate_error) in expected_errors
            else:
                assert repr(candidate_error) == repr(baseline_error)
        else:
            raise AssertionError(
                f"{candidate_name} expected to produce {baseline_error!r} exception,"
                f" but instead returned {candidate_value!r}"
            ) from baseline_error
    else:
        try:
            candidate_value = copy.deepcopy(get_obj(), **memo_kwargs(memo))
        except Exception as unexpected_candidate_error:
            raise AssertionError(
                f"{candidate_name} failed unexpectedly when copy.deepcopy didn't"
            ) from unexpected_candidate_error

        assert_equivalent_transformations(
            get_obj(),
            baseline_value,
            candidate_value,
        )


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


def test_graceful_recursion_error():
    value = make_nested(999999)
    with pytest.raises(RecursionError):
        copium.deepcopy(value)  # without safeguards this can SIGSEGV


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


def test_memo_stolen_ref_cycle_garbage_collected(copy):
    collected = set()

    class Thief:
        def __deepcopy__(self, memo):
            # creates cycle and prevents
            self.memo = memo

        def __del__(self):
            # won't run if memo is not tracked by gc
            collected.add(id(self))

    copy.deepcopy(thief := Thief())

    thief_id = id(thief)

    assert thief_id not in collected

    gc.disable()
    time.sleep(0.01)

    assert thief_id not in collected

    del thief

    gc.enable()
    gc.collect()

    assert thief_id in collected

    assert copy.deepcopy([]) == []


class DeepcopyRuntimeError:
    """
    A value that mutates its host when deep-copied.
    """

    def __init__(self, mutate) -> None:
        self._mutate = mutate

    def __deepcopy__(self, memo: dict):
        self._mutate()
        return self


def test_duper_deepcopy_parity_threaded_mutating() -> None:
    """
    This test is kind of flaky on free-threaded builds.
    """
    from concurrent.futures import ALL_COMPLETED
    from concurrent.futures import ThreadPoolExecutor
    from concurrent.futures import wait

    threads = 10
    repeats = 10
    total_runs = threads * repeats

    def run(copy):
        value: dict[Any, Any] = {}

        def mutate():
            value[random.randbytes(100)] = 1

        value["trigger"] = DeepcopyRuntimeError(mutate)

        def assert_runtime_error():
            try:
                copy.deepcopy(value)
            except RuntimeError:
                return True
            return False

        with ThreadPoolExecutor(max_workers=threads) as pool:
            futures = [pool.submit(assert_runtime_error) for _ in range(total_runs)]

            done, not_done = wait(futures, return_when=ALL_COMPLETED)
            assert not not_done
            return sum(f.result() for f in done)

    stdlib_runs = [run(stdlib_copy) for _ in range(10)]
    copium_runs = [run(copium) for _ in range(10)]
    assert sum(copium_runs) == pytest.approx(sum(stdlib_runs), abs=50)


def test_deepcopy_detects_self_mutation(copy) -> None:
    """Each thread gets its own dict that mutates itself during iteration."""
    from concurrent.futures import ALL_COMPLETED
    from concurrent.futures import ThreadPoolExecutor
    from concurrent.futures import wait

    threads = 10
    repeats = 10

    def run_once():
        local_dict = {}

        def mutate():
            local_dict[object()] = 1  # guaranteed unique key

        local_dict["a"] = 1
        local_dict["b"] = 1
        local_dict["trigger"] = DeepcopyRuntimeError(mutate)

        try:
            copy.deepcopy(local_dict)
        except RuntimeError:
            return True
        else:
            return False  # should have raised

    with ThreadPoolExecutor(max_workers=threads) as pool:
        total_runs = threads * repeats
        futures = [pool.submit(run_once) for _ in range(total_runs)]
        done, _ = wait(futures, return_when=ALL_COMPLETED)

        failures = [f for f in done if not f.result()]
        assert not failures, f"{len(failures)}/{total_runs} runs didn't raise RuntimeError"


def test_cross_thread_mutation_detection(copy) -> None:
    iterator_ready = threading.Event()
    mutation_done = threading.Event()

    class PausingValue:
        def __deepcopy__(self, memo):
            iterator_ready.set()
            mutation_done.wait(timeout=5)
            return self

    correctly_handled = 0
    total_attempts = 20
    for _ in range(total_attempts):  # repeat to catch races
        iterator_ready.clear()
        mutation_done.clear()

        shared_dict = {"a": 1, "pause": PausingValue()}
        result = {"raised": False}

        def iterate():
            try:
                copy.deepcopy(shared_dict)  # noqa: B023  # does not bind loop variable
            except RuntimeError:
                result["raised"] = True  # noqa: B023  # does not bind loop variable

        def mutate():
            iterator_ready.wait(timeout=5)
            shared_dict["injected"] = 1  # noqa: B023  # does not bind loop variable
            mutation_done.set()

        t_iter = threading.Thread(target=iterate)
        t_mutate = threading.Thread(target=mutate)

        t_iter.start()
        t_mutate.start()
        t_iter.join(timeout=10)
        t_mutate.join(timeout=10)

        shared_dict.pop("injected", None)

        correctly_handled += result["raised"]

    assert correctly_handled == total_attempts
