# Memory Leak Tests

This document explains the memory leak testing methodology and how to run the tests.

## Overview

The memory leak tests (`test_memory_leaks.py`) ensure that copium doesn't leak memory during deepcopy operations. These tests are particularly important because copium uses a Thread-Local Storage (TLS) growable buffer for performance optimization.

## Methodology

### Challenge

copium uses a TLS growable buffer for the memo table that is intentionally retained between deepcopy calls for performance. This is **not a memory leak** - it's a deliberate optimization. The buffer:

1. Grows to accommodate workload (up to `COPIUM_MEMO_RETAIN_MAX_SLOTS` = 128K slots)
2. Is reused across deepcopy calls (cleared but not freed)
3. Shrinks back to `COPIUM_MEMO_RETAIN_SHRINK_TO` (8K slots) when exceeding limits

### What We Test

1. **Peak Memory Comparison**: Memory allocated during `copium.deepcopy()` MUST be ≤ `stdlib.deepcopy()`
   - Measured both at Python level (`tracemalloc`) and process level (`psutil`)
   - Ensures copium isn't using MORE memory than stdlib for the same operation

2. **Memory Stability**: After initial TLS buffer growth, memory should stabilize
   - No unbounded memory growth over many iterations
   - Retention policy prevents continuous growth

3. **Proper Cleanup**: After deepcopy completes, only the retention buffer remains
   - Copied objects don't leak references
   - Memory returns to baseline after garbage collection

4. **Retention Policy**: Buffer shrinks correctly when exceeding limits
   - Large operations trigger growth
   - Subsequent small operations trigger shrinkage

### Test Cases

**Parametrized Tests** (run for all memo options × all CASE_PARAMS):
- `test_peak_memory_comparison_tracemalloc`: Compare Python-level memory (tracemalloc)
  - Tests: 6 memo options × ~100+ test cases from datamodelzoo = 600+ combinations
- `test_peak_memory_comparison_psutil`: Compare process memory including C allocations (psutil)
  - Tests: 6 memo options × ~100+ test cases from datamodelzoo = 600+ combinations

**Memo options tested:**
- `absent`: No memo provided (uses TLS buffer optimization)
- `dict`: Explicit dict memo (no TLS optimization)
- `None`: Explicit None memo (uses TLS optimization)
- `mapping`: UserDict memo (MutableMapping)
- `mutable_mapping`: MappingProxyType (expected to fail)
- `invalid`: Invalid memo type (expected to fail)

**Strict enforcement:** All memo types enforce **<=0% overhead** (copium memory must be <= stdlib).
After optimizing the C code to use `PyObject_CallMethodObjArgs` with cached interned strings,
custom memo types have zero overhead compared to stdlib.

**Noise filtering:** Each measurement runs 5 times with a warmup, taking the MINIMUM to filter out
measurement noise. Noise can only ADD overhead (page faults, allocator overhead), so the minimum
approaches the true memory usage.

**Known issue:** Tests show a consistent 24-byte overhead for `memo_dict`, `memo_mapping`, and
`memo_mutable_mapping` cases. The root cause is unknown. This exact overhead is subtracted in
comparisons to maintain strict <=0% enforcement for actual memory leaks.

**Single-run Tests:**
- `test_memory_stability_no_unbounded_growth`: Verify no continuous growth over iterations
- `test_memory_cleanup_after_deepcopy`: Verify proper cleanup after operations
- `test_no_reference_leaks`: Check for reference count leaks
- `test_large_data_retention_policy`: Verify buffer shrinkage policy
- `test_memo_dict_no_tls_optimization`: Test behavior with explicit memo dict
- `test_concurrent_deepcopy_memory_isolation`: Verify TLS isolation between threads
  - Note: Verifies correctness, not memory measurement (tracemalloc is global)

## Running the Tests

### Prerequisites

Install test dependencies including psutil:

```bash
task setup  # or: uv sync --extra test
```

### Running Memory Tests

Memory tests are **opt-in** via the `--memory` flag because they are:
- Resource-intensive
- Slower than regular tests
- May have platform-specific behavior

```bash
# Run all memory tests
pytest --memory tests/test_memory_leaks.py -v

# Run specific memory test
pytest --memory tests/test_memory_leaks.py::test_peak_memory_comparison_tracemalloc -v

# Run with additional pytest options
pytest --memory tests/test_memory_leaks.py -v -s  # show print statements
```

### Without --memory Flag

By default, memory tests are skipped:

```bash
# These will be skipped
pytest tests/test_memory_leaks.py -v
# Output: SKIPPED (need --memory option to run)
```

## Dependencies

- **tracemalloc**: Built-in Python module for measuring Python allocations
- **psutil**: External package for measuring process memory (RSS)
  - More accurate for C-level allocations
  - Required for some tests

If psutil is not installed, tests that require it will be skipped automatically.

## Interpreting Results

### Expected Behavior

1. **Peak Memory**: copium should use ≤ memory than stdlib
   - Small margin (10-20%) allowed for measurement noise

2. **Stability**: After ~10-20 iterations, memory should stabilize
   - Some growth is expected initially (TLS buffer allocation)
   - Later iterations should not show continuous growth

3. **Cleanup**: After deleting references and running GC:
   - Most memory should be freed
   - Only retention buffer (< 5 MB) should remain

### Troubleshooting

If tests fail:

1. **Memory comparison fails**: Check if copium is using significantly more memory
   - May indicate a real leak or inefficiency
   - Check C code for missing DECREFs

2. **Stability test fails**: Memory keeps growing
   - May indicate retention policy not working
   - Check buffer shrinkage logic in `_memo.c`

3. **Cleanup test fails**: Memory not released
   - May indicate reference leak
   - Check for cycles or missing cleanup

## Platform Considerations

Memory measurements can vary by platform:
- Different malloc implementations
- Different memory fragmentation behavior
- Different GC behavior

Tests include margins to account for this variability.

## CI/CD Integration

Memory tests can be run in CI but may be flaky due to:
- Shared resources
- Platform differences
- GC timing

Consider running them separately or with retries:

```bash
pytest --memory tests/test_memory_leaks.py --maxfail=1 -x
```

## Future Improvements

Potential enhancements:
- Memory profiling integration (memray, guppy3)
- Leak detection tools (valgrind, AddressSanitizer)
- More granular C-level allocation tracking
- Performance benchmarking alongside memory testing
