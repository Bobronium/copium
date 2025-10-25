# copium

An extremely fast Python copy implementation, written in C.

An extremely fast Python deepcopy implementation, written in C.

## Installation

```bash
pip install copyc
```

## Usage

### You have two choices:

1. Just patch builtin deepcopy and get on with your life. Make sure this is run in all processes and you're golden:

```python
import copyc.patch


copyc.patch.enable()
```

2. Use copyc directly

```python
from copyc import deepcopy


deepcopy()
```

## Development

- Install Task: https://taskfile.dev
- First run: `task setup`
- All checks: `task`
