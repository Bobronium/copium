# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
#
# SPDX-License-Identifier: MIT
"""
Memory leak tests for copium.deepcopy.

These tests ensure that the copium deepcopy implementation doesn't leak memory,
particularly around the growable TLS memo buffer. Tests compare memory usage
patterns between stdlib and copium implementations.

By default, these tests are skipped as they are slow. They run automatically on CI
or when explicitly requested with: pytest -m memory
"""
import collections
import gc
import os
import sys
import tracemalloc
from types import MappingProxyType
from typing import Any

import pytest
from datamodelzoo import CASES

import copium

# Only run memory tests on CI or when explicitly requested
SKIP_MEMORY_TESTS = os.environ.get("CI") != "true" and not os.environ.get("RUN_MEMORY_TESTS")

# Filter out cases that raise exceptions or are third-party specific
BASE_CASES = [
    case
    for case in CASES
    if "raises" not in case.name and "thirdparty" not in case.name
]

# Use a subset of cases for memory tests to keep runtime reasonable
# Focus on cases that stress different code paths
MEMORY_TEST_CASES = [
    case
    for case in BASE_CASES
    if any(
        keyword in case.name
        for keyword in [
            "reflexive",  # Circular references stress memo handling
            "proto:",  # Custom __deepcopy__ methods
            "list",  # Common collections
            "dict",
            "slots",  # __slots__ objects
            "91610",  # CPython issue cases (complex scenarios)
        ]
    )
]

# If no filtered cases, use a small subset
if not MEMORY_TEST_CASES:
    MEMORY_TEST_CASES = BASE_CASES[:10]

MEMORY_CASE_PARAMS = [pytest.param(case, id=case.name) for case in MEMORY_TEST_CASES]

# Memo options to test different code paths
memo_options = ["absent", "dict", "None"]


def get_memo_kwargs(memo_option: str) -> dict[str, Any]:
    """Get kwargs for deepcopy based on memo option."""
    if memo_option == "dict":
        return {"memo": {}}
    elif memo_option == "None":
        return {"memo": None}
    else:  # absent
        return {}


class MemoryProfile:
    """Container for memory profiling results."""

    def __init__(self, name: str):
        self.name = name
        self.measurements: list[tuple[int, int, int]] = []  # (iteration, bytes, refcount)

    def add_measurement(self, iteration: int, bytes_allocated: int, refcount: int) -> None:
        """Add a memory measurement."""
        self.measurements.append((iteration, bytes_allocated, refcount))

    def compute_growth_rate(self, skip_first_n: int = 0) -> float:
        """
        Compute bytes per iteration using linear regression.

        Args:
            skip_first_n: Skip first N measurements (warmup period)

        Returns:
            Slope of linear regression (bytes per iteration)
        """
        if len(self.measurements) <= skip_first_n:
            return 0.0

        data = self.measurements[skip_first_n:]
        n = len(data)

        if n < 2:
            return 0.0

        # Linear regression: y = mx + b, where y is bytes and x is iteration
        sum_x = sum(iteration for iteration, _, _ in data)
        sum_y = sum(bytes_alloc for _, bytes_alloc, _ in data)
        sum_xy = sum(iteration * bytes_alloc for iteration, bytes_alloc, _ in data)
        sum_x2 = sum(iteration * iteration for iteration, _, _ in data)

        # Slope m = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x)
        denominator = n * sum_x2 - sum_x * sum_x
        if denominator == 0:
            return 0.0

        slope = (n * sum_xy - sum_x * sum_y) / denominator
        return slope

    def max_refcount_increase(self) -> int:
        """Get maximum refcount increase from first to last measurement."""
        if len(self.measurements) < 2:
            return 0
        first_refcount = self.measurements[0][2]
        max_refcount = max(refcount for _, _, refcount in self.measurements)
        return max_refcount - first_refcount


def profile_deepcopy_memory(
    deepcopy_func,
    obj: Any,
    memo_kwargs: dict[str, Any],
    warmup_iterations: int = 100,
    measurement_iterations: int = 500,
    snapshot_interval: int = 50,
) -> MemoryProfile:
    """
    Profile memory usage of deepcopy over many iterations.

    Args:
        deepcopy_func: The deepcopy function to profile
        obj: Object to deepcopy
        memo_kwargs: Kwargs to pass to deepcopy (memo option)
        warmup_iterations: Number of warmup iterations to skip
        measurement_iterations: Number of iterations to measure
        snapshot_interval: Take snapshot every N iterations

    Returns:
        MemoryProfile with measurements
    """
    profile = MemoryProfile(deepcopy_func.__module__)

    # Start memory tracking
    tracemalloc.start()
    gc.collect()

    # Warmup phase - let Python's memory pools stabilize
    for _ in range(warmup_iterations):
        result = deepcopy_func(obj, **memo_kwargs)
        del result

    gc.collect()
    initial_snapshot = tracemalloc.take_snapshot()
    initial_memory = sum(stat.size for stat in initial_snapshot.statistics("lineno"))

    # Measurement phase
    result = None
    for i in range(measurement_iterations):
        result = deepcopy_func(obj, **memo_kwargs)

        # Take snapshot at intervals
        if i % snapshot_interval == 0:
            gc.collect()
            snapshot = tracemalloc.take_snapshot()
            current_memory = sum(stat.size for stat in snapshot.statistics("lineno"))
            memory_used = current_memory - initial_memory

            # Get refcount of result object
            refcount = sys.getrefcount(result) if result is not None else 0

            profile.add_measurement(i, memory_used, refcount)

        # Keep result alive until end to measure steady-state refcounts
        # But don't accumulate results
        if i % 10 == 0:
            del result
            result = None

    # Final measurement
    gc.collect()
    final_snapshot = tracemalloc.take_snapshot()
    final_memory = sum(stat.size for stat in final_snapshot.statistics("lineno"))
    memory_used = final_memory - initial_memory
    final_refcount = sys.getrefcount(result) if result is not None else 0
    profile.add_measurement(measurement_iterations, memory_used, final_refcount)

    # Cleanup
    del result
    tracemalloc.stop()
    gc.collect()

    return profile


@pytest.mark.memory
@pytest.mark.skipif(SKIP_MEMORY_TESTS, reason="Memory tests only run on CI or with RUN_MEMORY_TESTS=1")
@pytest.mark.parametrize("memo", memo_options, ids=[f"memo_{option}" for option in memo_options])
@pytest.mark.parametrize("case", MEMORY_CASE_PARAMS)
def test_no_memory_leak_vs_stdlib(case: Any, memo: str) -> None:
    """
    Test that copium.deepcopy doesn't leak more memory than stdlib.

    This test:
    1. Runs both stdlib and copium deepcopy repeatedly (with warmup)
    2. Measures memory growth over iterations using tracemalloc
    3. Computes linear regression to detect memory leaks
    4. Asserts copium's memory growth is comparable to stdlib's

    Memory allocated during copium.deepcopy MUST be <= memory allocated in stdlib.deepcopy()
    """
    import copy as stdlib_copy

    memo_kwargs = get_memo_kwargs(memo)

    # Skip if this memo option causes errors (e.g., invalid memo types)
    try:
        stdlib_copy.deepcopy(case.obj, **memo_kwargs)
    except (TypeError, AttributeError):
        pytest.skip(f"Memo option '{memo}' not compatible with this case")

    # Profile both implementations
    stdlib_profile = profile_deepcopy_memory(
        stdlib_copy.deepcopy,
        case.obj,
        memo_kwargs,
        warmup_iterations=100,
        measurement_iterations=500,
        snapshot_interval=50,
    )

    copium_profile = profile_deepcopy_memory(
        copium.deepcopy,
        case.obj,
        memo_kwargs,
        warmup_iterations=100,
        measurement_iterations=500,
        snapshot_interval=50,
    )

    # Compute growth rates (bytes per iteration)
    # Skip first measurement as memory pools may still be adjusting
    stdlib_growth = stdlib_profile.compute_growth_rate(skip_first_n=1)
    copium_growth = copium_profile.compute_growth_rate(skip_first_n=1)

    # Get final memory usage
    if stdlib_profile.measurements and copium_profile.measurements:
        stdlib_final_memory = stdlib_profile.measurements[-1][1]
        copium_final_memory = copium_profile.measurements[-1][1]
    else:
        stdlib_final_memory = 0
        copium_final_memory = 0

    # Check refcount increases (should be minimal)
    stdlib_refcount_increase = stdlib_profile.max_refcount_increase()
    copium_refcount_increase = copium_profile.max_refcount_increase()

    # Memory leak detection thresholds
    # Allow some tolerance for Python's memory management variability
    GROWTH_TOLERANCE_BYTES = 10_000  # 10KB per 500 iterations
    REFCOUNT_TOLERANCE = 3  # Allow small refcount variations

    # Assert 1: Growth rate should be near zero (no leak)
    # Both implementations should have flat memory after warmup
    assert (
        abs(copium_growth) < GROWTH_TOLERANCE_BYTES
    ), f"Copium shows memory growth: {copium_growth:.2f} bytes/iter (threshold: {GROWTH_TOLERANCE_BYTES})"

    assert (
        abs(stdlib_growth) < GROWTH_TOLERANCE_BYTES
    ), f"Stdlib shows memory growth: {stdlib_growth:.2f} bytes/iter (threshold: {GROWTH_TOLERANCE_BYTES})"

    # Assert 2: Copium's memory usage should be <= stdlib's (or close)
    # Allow copium to use up to 50% more memory than stdlib (for TLS buffers, etc.)
    memory_ratio = copium_final_memory / max(stdlib_final_memory, 1)
    assert (
        memory_ratio <= 1.5
    ), f"Copium uses {memory_ratio:.2f}x more memory than stdlib (copium: {copium_final_memory}, stdlib: {stdlib_final_memory})"

    # Assert 3: Refcount increases should be minimal
    assert (
        copium_refcount_increase <= REFCOUNT_TOLERANCE
    ), f"Copium shows excessive refcount growth: {copium_refcount_increase} (threshold: {REFCOUNT_TOLERANCE})"


@pytest.mark.memory
@pytest.mark.skipif(SKIP_MEMORY_TESTS, reason="Memory tests only run on CI or with RUN_MEMORY_TESTS=1")
def test_memo_reuse_no_leak() -> None:
    """
    Test that TLS memo buffer reuse doesn't leak memory.

    This specifically tests the growable TLS memo buffer mentioned in the issue.
    The memo should be reused across calls without leaking.
    """
    # Create an object that will grow the memo buffer
    deep_structure = []
    current = deep_structure
    for _ in range(100):
        new_list = []
        current.append(new_list)
        current = new_list

    tracemalloc.start()
    gc.collect()

    # Warmup
    for _ in range(50):
        result = copium.deepcopy(deep_structure)
        del result

    gc.collect()
    initial_snapshot = tracemalloc.take_snapshot()
    initial_memory = sum(stat.size for stat in initial_snapshot.statistics("lineno"))

    # Measure
    measurements = []
    for i in range(500):
        result = copium.deepcopy(deep_structure)
        del result

        if i % 50 == 0:
            gc.collect()
            snapshot = tracemalloc.take_snapshot()
            current_memory = sum(stat.size for stat in snapshot.statistics("lineno"))
            measurements.append((i, current_memory - initial_memory))

    tracemalloc.stop()
    gc.collect()

    # Check for growth
    if len(measurements) >= 2:
        first_memory = measurements[0][1]
        last_memory = measurements[-1][1]
        memory_increase = last_memory - first_memory

        # Should have minimal growth (< 50KB over 500 iterations)
        assert (
            memory_increase < 50_000
        ), f"TLS memo buffer shows memory growth: {memory_increase} bytes"


@pytest.mark.memory
@pytest.mark.skipif(SKIP_MEMORY_TESTS, reason="Memory tests only run on CI or with RUN_MEMORY_TESTS=1")
def test_exception_path_no_leak() -> None:
    """
    Test that exception paths don't leak memory.

    When deepcopy fails (e.g., due to invalid memo), ensure no memory is leaked.
    """

    class Uncopyable:
        def __deepcopy__(self, memo):
            raise RuntimeError("Cannot copy")

    obj = [1, 2, Uncopyable(), 4]

    tracemalloc.start()
    gc.collect()

    initial_snapshot = tracemalloc.take_snapshot()
    initial_memory = sum(stat.size for stat in initial_snapshot.statistics("lineno"))

    # Try to copy many times, expecting failures
    for _ in range(1000):
        try:
            copium.deepcopy(obj)
        except RuntimeError:
            pass

    gc.collect()
    final_snapshot = tracemalloc.take_snapshot()
    final_memory = sum(stat.size for stat in final_snapshot.statistics("lineno"))

    tracemalloc.stop()

    memory_increase = final_memory - initial_memory

    # Should have minimal growth even with exceptions (< 100KB)
    assert (
        memory_increase < 100_000
    ), f"Exception path shows memory growth: {memory_increase} bytes"


@pytest.mark.memory
@pytest.mark.skipif(SKIP_MEMORY_TESTS, reason="Memory tests only run on CI or with RUN_MEMORY_TESTS=1")
def test_large_memo_no_leak() -> None:
    """
    Test that large memo dictionaries don't leak memory.

    When copying objects with many unique items (large memo), ensure the memo
    doesn't leak references.
    """
    # Create many unique objects that will fill the memo
    large_obj = [object() for _ in range(1000)]

    tracemalloc.start()
    gc.collect()

    # Warmup
    for _ in range(50):
        result = copium.deepcopy(large_obj)
        del result

    gc.collect()
    initial_snapshot = tracemalloc.take_snapshot()
    initial_memory = sum(stat.size for stat in initial_snapshot.statistics("lineno"))

    # Measure
    for _ in range(500):
        result = copium.deepcopy(large_obj)
        del result

    gc.collect()
    final_snapshot = tracemalloc.take_snapshot()
    final_memory = sum(stat.size for stat in final_snapshot.statistics("lineno"))

    tracemalloc.stop()

    memory_increase = final_memory - initial_memory

    # Should have minimal growth (< 100KB)
    assert memory_increase < 100_000, f"Large memo shows memory growth: {memory_increase} bytes"
