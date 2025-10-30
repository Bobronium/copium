# copium

[![image](https://img.shields.io/pypi/v/copium.svg)](https://pypi.python.org/pypi/copium)
[![image](https://img.shields.io/pypi/l/copium.svg)](https://pypi.python.org/pypi/copium)
[![image](https://img.shields.io/pypi/pyversions/copium.svg)](https://pypi.python.org/pypi/copium)
[![Actions status](https://github.com/Bobronium/copium/actions/workflows/build.yml/badge.svg)](https://github.com/Bobronium/copium/actions)

An extremely fast Python copy/deepcopy implementation, written in C.

<div align="center">
  <picture>
    <source srcset="https://raw.githubusercontent.com/Bobronium/copium/main/assets/chart_dark.svg" media="(prefers-color-scheme: dark)">
    <source srcset="https://raw.githubusercontent.com/Bobronium/copium/main/assets/chart_light.svg" media="(prefers-color-scheme: light)">
    <img src="https://raw.githubusercontent.com/Bobronium/copium/main/assets/chart_light.svg" alt="Benchmark results bar chart">
  </picture>
</div>

<div align="center">
  <i><code>deepcopy_memo</code> suite from <a href="https://github.com/python/pyperformance/blob/643526f166869c6006009d316be38a35a3cffb2c/pyperformance/data-files/benchmarks/bm_deepcopy/run_benchmark.py#L88">python/pyperformance</a></i>
</div>

## Highlights

- ~3x faster on mixed data
- ~6x faster on typical data
- [~30 faster in some cases](#benchmarks)
- [Requires **zero** code changes to adopt](#1-you-set-copium_patch_deepcopy1-before-launch)
- Passes all tests from `CPython/Lib/test/test_copy.py`

## Installation

```bash
pip install copium
```

## Usage

`copium` is designed to be drop-in replacement for `copy` module.

After installation deepcopy will be fast in either of 3 cases:

##### 1) You set `COPIUM_PATCH_DEEPCOPY=1` before launch

##### 2) You call `copium.patch.enable()` manually at launch

##### 3) You use `copium.deepcopy()` directly

Generally any code that uses stdlib `copy` can be replaced with `copium` simply by:

```diff
- from copy import deepcopy
+ from copium import deepcopy
```

Alternatively, you can import `copium.patch` once and enable it:

```python
import copium.patch


copium.patch.enable()
```

Or just `export COPIUM_PATCH_DEEPCOPY=1` before running your Python process.

This will automatically call `copium.patch.enable()` on start, and all calls to `copy.deepcopy()` will be forwarded to
`copium.deepcopy()`. On Python 3.12+ there's no performance overhead compared to direct
usage.

There are two main benefits of using `copium.patch`,

- It requires zero code changes
- It automatically makes any third party code that uses deepcopy faster, for
  instance, it will speed up instantiations of pydantic
  models with mutable defaults
  \(see [pydantic_core](https://github.com/pydantic/pydantic-core/blob/f1239f81d944bcda84bffec64527d46f041ccc9e/src/validators/with_default.rs#L23)).

## Caveats

- `copium.deepcopy()` ignores `sys.getrecursionlimit()`. It still may raise `RecursionError` at some point, but at much
  larger depths than default interpreter recursion limit (see `tests.test_copium.test_recursion_error`)
- unless `memo` argument supplied as `dict` when calling `copium.deepcopy()`, special lightweight memo storage will be
  used to reduce memoization overhead. It implements `MutableMapping` methods, so any custom `__deepcopy__` methods
  should work as expected
- `copium` uses unstable CPython API. This means that it might break on new major Python release

## Benchmarks

A full benchmark suite is in progress and will be published soon.
In the meanwhile, you can reproduce the results shown in the chart above with this minimal script

<details>
<summary>Pyperf case</summary>

```shell
cat > benchmark.py << 'PY'
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pyperf",
#     "copium",
# ]
# ///
import pyperf

runner = pyperf.Runner()

setup = """
import copy
from decimal import Decimal

payload = {
        "a": 1,
        "b": (b := [(1, 2, 3), (4, 5, 6)]),
        "c": [Decimal("3.14"), complex(), [], (), frozenset(), b],
}
"""

runner.timeit(name="deepcopy", stmt=f"b=copy.deepcopy(payload)", setup=setup)
PY
```

```shell
uv run --python 3.14t benchmark.py -q -o copy3.14t.json && \
COPIUM_PATCH_DEEPCOPY=1 PYTHON_GIL=0 \
uv run --python 3.14t benchmark.py -q -o copium3.14t.json --copy-env && \
uvx pyperf compare_to copy3.14t.json copium3.14t.json --table
```

Output:

```shell
deepcopy: Mean +- std dev: 20.8 us +- 1.6 us
deepcopy: Mean +- std dev: 928 ns +- 11 ns
+--------------+---------+--------------------+
| Benchmark | copy    | copium                |
+===========+=========+=======================+
| deepcopy  | 20.8 us | 928 ns: 22.40x faster |
+-----------+---------+-----------------------+
```

```shell
❯ uv run --python 3.13 benchmark.py -q -o copy3.13.json && \
COPIUM_PATCH_DEEPCOPY=1 \
uv run --python 3.13 benchmark.py -q -o copium3.13.json --copy-env && \
uvx pyperf compare_to copy3.13.json copium3.13.json --table
```

```shell
deepcopy: Mean +- std dev: 10.8 us +- 0.9 us
deepcopy: Mean +- std dev: 880 ns +- 23 ns
+-----------+-----------+-----------------------+
| Benchmark | copy3.13t | copium3.13t           |
+===========+===========+=======================+
| deepcopy  | 10.8 us   | 880 ns: 12.26x faster |
+-----------+-----------+-----------------------+
```

```shell
❯ uv run --python 3.13t benchmark.py -q -o copy3.13t.json && \
COPIUM_PATCH_DEEPCOPY=1 PYTHON_GIL=0 \
uv run --python 3.13t benchmark.py -q -o copium3.13t.json --copy-env && \
uvx pyperf compare_to copy3.13t.json copium3.13t.json --table
```

```shell
deepcopy: Mean +- std dev: 29.0 us +- 6.7 us
deepcopy: Mean +- std dev: 942 ns +- 29 ns
+-----------+-----------+-----------------------+
| Benchmark | copy3.13t | copium3.13t           |
+===========+===========+=======================+
| deepcopy  | 29.0 us   | 942 ns: 30.84x faster |
+-----------+-----------+-----------------------+
```

</details>

## Development

- Install Task: https://taskfile.dev
- All checks: `task` / `task MATRIX=1`
