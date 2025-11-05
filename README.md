# copium

[![PyPI Version Badge](https://img.shields.io/pypi/v/copium.svg)](https://pypi.python.org/pypi/copium)
[![PyPI License Badge](https://img.shields.io/pypi/l/copium.svg)](https://pypi.python.org/pypi/copium)
[![PyPI Python Versions Badge](https://img.shields.io/pypi/pyversions/copium.svg)](https://pypi.python.org/pypi/copium)
[![Actions status Badge](https://github.com/Bobronium/copium/actions/workflows/build.yaml/badge.svg)](https://github.com/Bobronium/copium/actions)
![Codspeed Badge](https://img.shields.io/badge/Codspeed-benchmarks-8A2BE2?style=flat&logo=data%3Aimage%2Fsvg%2Bxml%3Bbase64%2CPHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA0MCA0MCIgcHJlc2VydmVBc3BlY3RSYXRpbz0ieE1pZFlNaWQgbWVldCI%2BCiAgICA8ZyB0cmFuc2Zvcm09InRyYW5zbGF0ZSgtMTAwLDEwKSB0cmFuc2xhdGUoMTIwLDEyKSBzY2FsZSgxLjMpIHRyYW5zbGF0ZSgtMTIwLC0xMikiPiI%2BCiAgICAgICAgPHBhdGggZmlsbD0iI0VENkUzRCIgZmlsbC1ydWxlPSJldmVub2RkIgogICAgICAgICAgICAgIGQ9Ik0xMTAuMTIxIDE3LjExN2MuNzY2LjE3IDEuMzA4LjA1IDEuMzkyLjA2NGwuMDA0LjAwMWMxLjI3NS42OTEgMi4yMDIgMS4yNzkgMy4wOTcgMS42NTVsLS4xMDcuMDFxLTEuMDkyLjE3Mi0xLjU3Mi4yNWMtMy4yNzYuNTMzLTQuODg0LS4zOTgtNC41MzItMS44My4xNDItLjU3OC45MzgtLjMyNCAxLjcxOC0uMTVtMTEuMDA0LTEzLjkxYzIuMDc0IDEuNTM0IDIuNjcgMi4zMzEgMy43NzQgMy41NTUgMi43MDggMCA0LjIyIDIuMDI2IDMuNzM1IDUuMDQ2LS4zMDggMS45MjEtNC4xNSAxLjI0Ni01LjA2IDBxLS45MTIuODI2LTQuNDgzIDMuNjYzbC0uMDk3LjA3NmMtLjY5NS41NTMtMy4zNzcuMzc2LTMuNjM0LjE4N3EuODA2LTEuMzI1IDEuMTYxLTIuMDcyYy4zNTYtLjc0NS42MDUtMS40OTMuNjA1LTIuNzMyIDAtMS4yMzgtLjY5NS0yLjI5LTIuMTY2LTIuMjYzYS4yOC4yOCAwIDAgMC0uMjc0LjI5NWMwIC4xOTUuMTQ1LjI5Ni4yNzQuMjk2Ljc3OSAwIDEuMzI1Ljk2OCAxLjMyNSAxLjY3My4wMDEuNzA0LS4xMTEgMS4yNzUtLjQ0NCAyLjEzNC0uMjg3Ljc0MS0xLjQ0NCAyLjU4My0xLjc0NSAyLjc2N2EuMjc4LjI3OCAwIDAgMCAuMDQyLjQ4NnEuMDMxLjAxNS4wNzkuMDMuMS4wMzIuMjUzLjA3MWMuMjYyLjA2NC41ODEuMTIxLjk0LjE2My45ODcuMTEzIDIuMDk0LjA5IDMuMjc0LS4xMmwuMDQ1LS4wMDljLjM1Mi0uMDY0Ljg2NS0uMDY5IDEuMzcyLS4wMDMuNTkzLjA3OCAxLjEzMy4yNDQgMS41NDMuNDkzLjM2LjIxOC42MDguNDkuNzM1LjgybC4wMTIuMDM2cS4wOC4yNjMuMDguNTY2YzAgMS4wODMtMi4zMDguNDM0LTQuOTc2LjMxOGE5IDkgMCAwIDAtLjYxLS4wMDJjLTEuMDg5LS4wNTUtMS45ODUtLjM3NC0zLjE4Ni0uOTc0bC4wMjEtLjAwNHMtLjA5Mi0uMDM4LS4yMzgtLjEwNmMtLjM1Ni0uMTgyLS43NC0uMzg3LTEuMTYyLS42MTZoLS4wMDNjLS4zOTgtLjI0OC0uNzQ5LS41MjctLjgzOC0uNzc2LS4yMzMtLjY1MS0uMTE4LS42NTEuNzE1LTEuNjEzLTEuNDIyLjE3NS0xLjQ1Ny4yNzYtMy4wNzguMjc2cy00LjI5Mi0uMDgzLTQuMjkyLTEuNjdxMC0xLjU5IDIuMTYxLTEuMjM2LS41MjctMi44OSAxLjgwNy01LjJjNC4wNzYtNC4wMzUgOS41NzggMS41MjUgMTMuMzUgMS41MjUgMS43MTYgMC0zLjAyNS0yLjY5My00Ljk5NS0zLjQ1NnMxLjEzMS0zLjcyOSAzLjk3OC0xLjYyNG00Ljc0OCA1LjU1MmMtLjMxIDAtLjU2MS4yNy0uNTYxLjYwNXMuMjUxLjYwNC41NjEuNjA0LjU2MS0uMjcuNTYxLS42MDQtLjI1MS0uNjA1LS41NjEtLjYwNSIKICAgICAgICAgICAgICBjbGlwLXJ1bGU9ImV2ZW5vZGQiLz4KICAgIDwvZz4KPC9zdmc%2B&logoSize=auto&labelColor=1B2330&color=ED6E3D&link=https%3A%2F%2Fcodspeed.io%2FBobronium%2Fcopium)

An extremely fast Python copy/deepcopy implementation, written in C.

<div align="center">
  <picture>
    <source srcset="https://raw.githubusercontent.com/Bobronium/copium/102eb4a2ad26f0d6f22af6765c9f5b305ad24abb/assets/chart_dark.svg" media="(prefers-color-scheme: dark)">
    <source srcset="https://raw.githubusercontent.com/Bobronium/copium/102eb4a2ad26f0d6f22af6765c9f5b305ad24abb/assets/chart_light.svg" media="(prefers-color-scheme: light)">
    <img src="https://raw.githubusercontent.com/Bobronium/copium/102eb4a2ad26f0d6f22af6765c9f5b305ad24abb/assets/chart_light.svg" alt="Benchmark results bar chart">
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
