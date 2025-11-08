# copium

[![PyPI Version Badge](https://img.shields.io/pypi/v/copium.svg)](https://pypi.python.org/pypi/copium)
[![PyPI License Badge](https://img.shields.io/pypi/l/copium.svg)](https://pypi.python.org/pypi/copium)
[![PyPI Python Versions Badge](https://img.shields.io/pypi/pyversions/copium.svg)](https://pypi.python.org/pypi/copium)
[![Actions status Badge](https://github.com/Bobronium/copium/actions/workflows/build.yaml/badge.svg)](https://github.com/Bobronium/copium/actions)
[![Codspeed Badge](https://img.shields.io/badge/Codspeed-benchmarks-8A2BE2?style=flat&logo=data%3Aimage%2Fsvg%2Bxml%3Bbase64%2CPHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCA0MCA0MCIgcHJlc2VydmVBc3BlY3RSYXRpbz0ieE1pZFlNaWQgbWVldCI%2BCiAgICA8ZyB0cmFuc2Zvcm09InRyYW5zbGF0ZSgtMTAwLDEwKSB0cmFuc2xhdGUoMTIwLDEyKSBzY2FsZSgxLjMpIHRyYW5zbGF0ZSgtMTIwLC0xMikiPiI%2BCiAgICAgICAgPHBhdGggZmlsbD0iI0VENkUzRCIgZmlsbC1ydWxlPSJldmVub2RkIgogICAgICAgICAgICAgIGQ9Ik0xMTAuMTIxIDE3LjExN2MuNzY2LjE3IDEuMzA4LjA1IDEuMzkyLjA2NGwuMDA0LjAwMWMxLjI3NS42OTEgMi4yMDIgMS4yNzkgMy4wOTcgMS42NTVsLS4xMDcuMDFxLTEuMDkyLjE3Mi0xLjU3Mi4yNWMtMy4yNzYuNTMzLTQuODg0LS4zOTgtNC41MzItMS44My4xNDItLjU3OC45MzgtLjMyNCAxLjcxOC0uMTVtMTEuMDA0LTEzLjkxYzIuMDc0IDEuNTM0IDIuNjcgMi4zMzEgMy43NzQgMy41NTUgMi43MDggMCA0LjIyIDIuMDI2IDMuNzM1IDUuMDQ2LS4zMDggMS45MjEtNC4xNSAxLjI0Ni01LjA2IDBxLS45MTIuODI2LTQuNDgzIDMuNjYzbC0uMDk3LjA3NmMtLjY5NS41NTMtMy4zNzcuMzc2LTMuNjM0LjE4N3EuODA2LTEuMzI1IDEuMTYxLTIuMDcyYy4zNTYtLjc0NS42MDUtMS40OTMuNjA1LTIuNzMyIDAtMS4yMzgtLjY5NS0yLjI5LTIuMTY2LTIuMjYzYS4yOC4yOCAwIDAgMC0uMjc0LjI5NWMwIC4xOTUuMTQ1LjI5Ni4yNzQuMjk2Ljc3OSAwIDEuMzI1Ljk2OCAxLjMyNSAxLjY3My4wMDEuNzA0LS4xMTEgMS4yNzUtLjQ0NCAyLjEzNC0uMjg3Ljc0MS0xLjQ0NCAyLjU4My0xLjc0NSAyLjc2N2EuMjc4LjI3OCAwIDAgMCAuMDQyLjQ4NnEuMDMxLjAxNS4wNzkuMDMuMS4wMzIuMjUzLjA3MWMuMjYyLjA2NC41ODEuMTIxLjk0LjE2My45ODcuMTEzIDIuMDk0LjA5IDMuMjc0LS4xMmwuMDQ1LS4wMDljLjM1Mi0uMDY0Ljg2NS0uMDY5IDEuMzcyLS4wMDMuNTkzLjA3OCAxLjEzMy4yNDQgMS41NDMuNDkzLjM2LjIxOC42MDguNDkuNzM1LjgybC4wMTIuMDM2cS4wOC4yNjMuMDguNTY2YzAgMS4wODMtMi4zMDguNDM0LTQuOTc2LjMxOGE5IDkgMCAwIDAtLjYxLS4wMDJjLTEuMDg5LS4wNTUtMS45ODUtLjM3NC0zLjE4Ni0uOTc0bC4wMjEtLjAwNHMtLjA5Mi0uMDM4LS4yMzgtLjEwNmMtLjM1Ni0uMTgyLS43NC0uMzg3LTEuMTYyLS42MTZoLS4wMDNjLS4zOTgtLjI0OC0uNzQ5LS41MjctLjgzOC0uNzc2LS4yMzMtLjY1MS0uMTE4LS42NTEuNzE1LTEuNjEzLTEuNDIyLjE3NS0xLjQ1Ny4yNzYtMy4wNzguMjc2cy00LjI5Mi0uMDgzLTQuMjkyLTEuNjdxMC0xLjU5IDIuMTYxLTEuMjM2LS41MjctMi44OSAxLjgwNy01LjJjNC4wNzYtNC4wMzUgOS41NzggMS41MjUgMTMuMzUgMS41MjUgMS43MTYgMC0zLjAyNS0yLjY5My00Ljk5NS0zLjQ1NnMxLjEzMS0zLjcyOSAzLjk3OC0xLjYyNG00Ljc0OCA1LjU1MmMtLjMxIDAtLjU2MS4yNy0uNTYxLjYwNXMuMjUxLjYwNC41NjEuNjA0LjU2MS0uMjcuNTYxLS42MDQtLjI1MS0uNjA1LS41NjEtLjYwNSIKICAgICAgICAgICAgICBjbGlwLXJ1bGU9ImV2ZW5vZGQiLz4KICAgIDwvZz4KPC9zdmc%2B&logoSize=auto&labelColor=1B2330&color=ED6E3D&link=https%3A%2F%2Fcodspeed.io%2FBobronium%2Fcopium)](https://codspeed.io/Bobronium/copium)

Fast drop-in replacement for `copy.deepcopy()`.

> [!WARNING]  
> This is alpha version, see [project status](#project-status).


<div align="center">
  <picture>
    <source srcset="https://raw.githubusercontent.com/Bobronium/copium/72f0278e972cb36d501b7e9c465d909c77c20480/assets/chart_dark.svg" media="(prefers-color-scheme: dark)">
    <source srcset="https://raw.githubusercontent.com/Bobronium/copium/72f0278e972cb36d501b7e9c465d909c77c20480/assets/chart_light.svg" media="(prefers-color-scheme: light)">
    <img src="https://raw.githubusercontent.com/Bobronium/copium/72f0278e972cb36d501b7e9c465d909c77c20480/assets/chart_light.svg" alt="Benchmark results bar chart">
  </picture>
</div>

<div align="center">
  <i>Benchmarked on <a href="https://github.com/Bobronium/copium/actions/workflows/build.yaml">GitHub Actions</a> using <a href="https://github.com/Bobronium/copium/blob/ea038c2c1ba7ba84ff62982b2cc2a7d1326a510b/tools/run_benchmark.py">tools/run_benchmark.py</a>, charted with <a href="https://github.com/Bobronium/copium/blob/ea038c2c1ba7ba84ff62982b2cc2a7d1326a510b/tools/generate_chart.py">tools/generate_chart.py</a></i>
</div>


## Installation

```bash
pip install copium
```

## Usage

Simply export `COPIUM_PATCH_DEEPCOPY=1` before running your application. You
don't have to change a single line of code:

`cat example.py`

```python
from copy import deepcopy

assert deepcopy(x := []) is not x
```

`COPIUM_PATCH_DEEPCOPY=1 python example.py`

---

To enable patch manually:

```py
import copium.patch

copium.patch.enable()
```

---

To use manually:

```py
from copium import deepcopy

assert deepcopy(x := []) is not x
```

`copium` module includes all public declarations of stdlib `copy` module, so it's generally safe to:

```diff
- from copy import deepcopy
+ from copium import deepcopy
```

---

## Project status

- [x] passes all tests in `CPython/Lib/test/test_copy.py`
- [x] behaves exactly the same as stdlib `copy` in all cases declared in
  [datamodelzoo](https://github.com/Bobronium/datamodelzoo)
- [x] consumes less or equal amount of memory as stdlib `copy`
- [x] always faster than stdlib `copy`
- [x] passes all tests on free-threaded Python builds
- [x] has tests for refcounts/recursion/threading edge cases

If you're interested in using this, simply add it to your project and run tests with `COPIUM_PATCH_DEEPCOPY=1`.

If it fails — open an issue with detailed bugreport.

If it works as expected, consider whether the speedup in your case worth of using alpha version.


## Speedups on Python 3.14

### `tuple`

- `()` ~5.66x
- `tuple[int, ...]` ~12.46x (20 items)
- `tuple[int, ...]` ~28.28x (5000 items)
- `tuple[tuple[str, tuple[tuple[str, tuple[tuple[str, tuple[tuple[str, tuple[int, ...]]]]]]]]]]` ~14.54x

### `list`

- `[]` ~4.97x
- `list[int]` ~9.58x (20 items)
- `list[int]` ~16.23x (5000 items)
- `list[str | list[str | list[str | list[str | list[int]]]]]` ~9.19x

### `dict`

- `{}` ~4.59x
- `dict[str, int]` ~5.95x (20 items)
- `dict[str, int]` ~3.94x (5000 items)
- `dict[str, dict[str, ...]]` ~6.19x

### `set`

- `set()` ~15.87x
- `set[int]` ~6.55x (20 items)
- `set[int]` ~3.69x (5000 items)
- `set[frozenset[frozenset[int]]]` ~13.72x

### other

- `int`/`str`/`bytes` ~4.86x
- `dataclasses` ~2.74x

Expect much higher speedups on Python 3.13 and lower.
