"""
copium.deepcopy benchmark suite for CodSpeed.

Each synthetic group isolates one code path in the deepcopy pipeline.
Within a group, variants share identical structure but differ in the
measured signal.  3+ scale points per signal.  Real-world cases detect
end-to-end regression across representative workloads.

Deepcopy pipeline (from deepcopium.rs):

  1. pre-memo atomic?  → return immediately  (None/int/str/bool/float/bytes)
  2. memo recall       → hit: return cached; miss: continue
  3. type dispatch     → tuple / dict / list / set (exact type)
  4. post-memo atomic? → return immediately  (re.Pattern/type/range/function/…)
  5. specialized       → frozenset / bytearray / bound method
  6. reduce fallback   → __deepcopy__ or __reduce_ex__
"""

import copy as stdlib_copy
import os
import platform
import re
import sys
from dataclasses import dataclass
from dataclasses import field
from datetime import datetime
from datetime import timedelta
from itertools import chain
from typing import Any
from typing import NamedTuple

import pytest

import copium
import copium.patch


class Case(NamedTuple):
    name: str
    obj: Any
    memory: bool = True


def scaled(tag, factory, sizes):
    return (Case(f"{tag}-n-{n}", factory(n), memory=n >= 1000) for n in sizes)


def depth_scaled(tag, factory, depths):
    return (Case(f"{tag}-d-{d}", factory(d), memory=d >= 100) for d in depths)


CODSPEED_MEMORY = bool(os.getenv("CODSPEED_MEMORY"))


def generate_params(cases):
    return pytest.mark.parametrize(
        "case",
        (pytest.param(c, id=c.name) for c in cases if not CODSPEED_MEMORY or c.memory),
    )


python_version = ".".join(map(str, sys.version_info[:2]))
if not getattr(sys, "_is_gil_enabled", lambda: True)():
    python_version += "t"
python_version += f"-{platform.machine()}"

PYTHON_VERSION = pytest.mark.parametrize("_python", [python_version])

SIZES = (10, 100, 1000)
DEPTHS = (10, 100, 500)
ATOM_SIZES = (100, 1000, 10000)
REDUCE_SIZES = (10, 50, 200)


# ═══════════════════════════════════════════════════════════
#  MEMO ISOLATION
#
#  Constant shape: {'a': (X, X, X), 'b': [X] * n}
#
#  Outer dict (2 keys) and inner tuple/list are the same
#  across all variants.  X controls which memo path fires:
#
#  shared_mut         →  memo hit after first (shallow leaf)
#  shared_deep        →  memo hit after first (recursive leaf)
#  shared_tuple_atom  →  tuple all_same path, never memoised
#  shared_tuple_mut   →  tuple content changes → memo store + hits
#  shared_atom        →  pre-memo atomic skip, no memo
#  unique_atom        →  pre-memo atomic skip, distinct id()s
#  unique_mut         →  memo store each, zero hits
# ═══════════════════════════════════════════════════════════


def memo_shared_mut(n):
    leaf = [1, 2, 3]
    return {"a": (leaf, leaf, leaf), "b": [leaf] * n}


def memo_shared_deep(n):
    leaf = [[1, 2], {"k": "v"}, [3, 4]]
    return {"a": (leaf, leaf, leaf), "b": [leaf] * n}


def memo_shared_tuple_atom(n):
    leaf = (1, 2, 3)
    return {"a": (leaf, leaf, leaf), "b": [leaf] * n}


def memo_shared_tuple_mut(n):
    leaf = ([],)
    return {"a": (leaf, leaf, leaf), "b": [leaf] * n}


def memo_shared_atom(n):
    return {"a": (None, None, None), "b": [None] * n}


def memo_unique_atom(n):
    return {"a": (1, 2, 3), "b": list(range(n))}


def memo_unique_mut(n):
    return {"a": ([], [], []), "b": [[] for _ in range(n)]}


MEMO_CASES = chain(
    scaled("shared_mut", memo_shared_mut, SIZES),
    scaled("shared_deep", memo_shared_deep, SIZES),
    scaled("shared_tuple_atom", memo_shared_tuple_atom, SIZES),
    scaled("shared_tuple_mut", memo_shared_tuple_mut, SIZES),
    scaled("shared_atom", memo_shared_atom, SIZES),
    scaled("unique_atom", memo_unique_atom, SIZES),
    scaled("unique_mut", memo_unique_mut, SIZES),
)

# ═══════════════════════════════════════════════════════════
#  CONTAINER TRAVERSAL
#
#  Flat container of n atomic ints.
#  Isolates per-container creation + traversal cost.
# ═══════════════════════════════════════════════════════════

CONTAINER_CASES = chain(
    scaled("list", lambda n: list(range(n)), SIZES),
    scaled("tuple", lambda n: tuple(range(n)), SIZES),
    scaled("dict", lambda n: {i: i for i in range(n)}, SIZES),
    scaled("set", lambda n: set(range(n)), SIZES),
    scaled("frozenset", lambda n: frozenset(range(n)), SIZES),
    scaled("bytearray", lambda n: bytearray(n), (100, 10_000, 1_000_000)),
)


# ═══════════════════════════════════════════════════════════
#  NESTING DEPTH
#
#  Single chain d levels deep.  Leaf = [1, 2, 3] (mutable)
#  except tuple_atom which uses atomic leaf to trigger
#  the all_same optimisation at every level.
# ═══════════════════════════════════════════════════════════


def nested_list(d):
    obj = [1, 2, 3]
    for _ in range(d):
        obj = [obj]
    return obj


def nested_dict(d):
    obj = [1, 2, 3]
    for _ in range(d):
        obj = {"k": obj}
    return obj


def nested_tuple_mut(d):
    obj = [1, 2, 3]
    for _ in range(d):
        obj = (obj,)
    return obj


def nested_tuple_atom(d):
    obj = 42
    for _ in range(d):
        obj = (obj,)
    return obj


DEPTH_CASES = chain(
    depth_scaled("list", nested_list, DEPTHS),
    depth_scaled("dict", nested_dict, DEPTHS),
    depth_scaled("tuple_mut", nested_tuple_mut, DEPTHS),
    depth_scaled("tuple_atom", nested_tuple_atom, DEPTHS),
)

# ═══════════════════════════════════════════════════════════
#  ATOMIC FAST PATH
#
#  Outer list of n items.  List overhead is constant across
#  variants; we measure per-item dispatch cost.
#
#  Pre-memo atomics:  None, int, str, bool, float, bytes
#    → is_literal_immutable fires before memo
#  Post-memo atomics: re.Pattern, type objects
#    → memo recall miss, then is_postmemo_atomic fires
# ═══════════════════════════════════════════════════════════

CACHED_RE = re.compile(r"^test$")


def mixed_prememo_atoms(n):
    pool = [None, 42, "s", True, 3.14, b"b"]
    return [pool[i % 6] for i in range(n)]


ATOMIC_CASES = chain(
    scaled("none", lambda n: [None] * n, ATOM_SIZES),
    scaled("int", lambda n: list(range(n)), ATOM_SIZES),
    scaled("str", lambda n: [f"s{i}" for i in range(n)], ATOM_SIZES),
    scaled("mixed_builtin_atomics", mixed_prememo_atoms, ATOM_SIZES),
    scaled("re.Pattern", lambda n: [CACHED_RE] * n, ATOM_SIZES),
    scaled("type", lambda n: [int] * n, ATOM_SIZES),
)


# ═══════════════════════════════════════════════════════════
#  REDUCE PROTOCOL
#
#  Objects going through __reduce_ex__ / __deepcopy__.
#  List of n instances to scale.
# ═══════════════════════════════════════════════════════════


@dataclass
class SimpleDataclass:
    x: int
    y: str


@dataclass
class MutableDataclass:
    x: int
    items: list = field(default_factory=list)
    mapping: dict = field(default_factory=dict)


@dataclass
class NestedDataclass:
    inner: SimpleDataclass
    items: list = field(default_factory=list)


class SlotsObject:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


class CustomDeepcopyObject:
    def __init__(self, v):
        self.v = v

    def __deepcopy__(self, memo):
        return CustomDeepcopyObject(stdlib_copy.deepcopy(self.v, memo))


REDUCE_CASES = chain(
    scaled(
        "dataclass_simple",
        lambda n: [SimpleDataclass(i, f"v{i}") for i in range(n)],
        REDUCE_SIZES,
    ),
    scaled(
        "dataclass_mutable",
        lambda n: [MutableDataclass(i, [i], {"k": i}) for i in range(n)],
        REDUCE_SIZES,
    ),
    scaled(
        "dataclass_nested",
        lambda n: [NestedDataclass(SimpleDataclass(i, f"v{i}"), [i]) for i in range(n)],
        REDUCE_SIZES,
    ),
    scaled(
        "slots",
        lambda n: [SlotsObject(i, f"v{i}", float(i)) for i in range(n)],
        REDUCE_SIZES,
    ),
    scaled(
        "datetime",
        lambda n: [datetime(2024, 1, 1) + timedelta(days=i) for i in range(n)],  # noqa: DTZ001
        REDUCE_SIZES,
    ),
    scaled(
        "custom_deepcopy",
        lambda n: [CustomDeepcopyObject([i]) for i in range(n)],
        REDUCE_SIZES,
    ),
)


# ═══════════════════════════════════════════════════════════
#  EDGE CASES
#
#  Structural pathologies: cycles, empties, dense sharing,
#  all_same tuples at scale.
# ═══════════════════════════════════════════════════════════


def make_cyclic_list():
    a = [1, 2, 3]
    a.append(a)
    return a


def make_cyclic_dict():
    d = {"k": "v"}
    d["self"] = d
    return d


def make_dense_refs():
    nodes = [[i] for i in range(50)]
    return [nodes[i % 50] for i in range(2500)]


def wide_dict(n):
    return {f"k{i}": [i] for i in range(n)}


EDGE_CASES = [
    Case("cyclic_list", make_cyclic_list()),
    Case("cyclic_dict", make_cyclic_dict()),
    Case("empties", [[], (), {}, set(), frozenset(), bytearray()]),
    Case("tuple_allsame_10k", (None,) * 10000),
    Case("tuple_alldiff_1k", tuple([] for _ in range(1000))),
    Case("dense_refs_50x50", make_dense_refs()),
    *scaled("wide_dict", wide_dict, (100, 1000, 5000)),
]


# ═══════════════════════════════════════════════════════════
#  REAL-WORLD
#
#  Representative production deepcopy patterns.
#  Data is self-contained and deterministic.
# ═══════════════════════════════════════════════════════════


def make_json_api_response():
    return {
        "status": "ok",
        "pagination": {"page": 1, "per_page": 20, "total": 142},
        "data": [
            {
                "id": i,
                "type": "user",
                "attributes": {
                    "name": f"User {i}",
                    "email": f"u{i}@x.com",
                    "active": i % 3 != 0,
                    "score": float(i * 17 % 100),
                    "tags": ["admin", "verified"] if i % 5 == 0 else ["user"],
                    "metadata": {"joined": "2024-01-15", "logins": i * 7},
                },
                "relationships": {
                    "team": {"data": {"type": "team", "id": i % 4}},
                    "projects": {"data": [{"type": "project", "id": i * 10 + j} for j in range(3)]},
                },
            }
            for i in range(20)
        ],
        "included": [
            {"type": "team", "id": t, "attributes": {"name": f"Team {t}"}} for t in range(4)
        ],
        "meta": {"request_id": "abc-123", "timing_ms": 42.5},
    }


def make_config_with_shared_defaults():
    defaults = {"timeout": 30, "retries": 3, "backoff": 1.5}
    return {
        "version": "2.1.0",
        "environments": {
            env: {
                "database": {
                    "host": f"db-{env}",
                    "port": 5432,
                    "pool_size": pool_size,
                    "options": defaults,
                },
                "cache": {"host": f"redis-{env}", "port": 6379, "options": defaults},
                "features": {
                    "oauth": env != "dev",
                    "debug": env == "dev",
                    "providers": ["google", "github"] if env != "dev" else [],
                },
            }
            for env, pool_size in [("dev", 2), ("staging", 5), ("prod", 20)]
        },
        "shared": {
            "origins": ["https://app.example.com", "https://api.example.com"],
            "headers": ("Content-Type", "Authorization", "X-Request-ID"),
            "error_codes": frozenset({400, 401, 403, 404, 500}),
        },
    }


def make_openapi_fragment():
    def schema(name, fields):
        return {
            "type": "object",
            "title": name,
            "properties": {f: {"type": t} for f, t in fields},
            "required": [f for f, _ in fields],
        }

    base_fields = [
        ("id", "integer"),
        ("name", "string"),
        ("created_at", "string"),
        ("updated_at", "string"),
        ("metadata", "object"),
    ]

    schemas = {}
    for model in ("User", "Project", "Task", "Comment"):
        schemas[model] = schema(model, base_fields)
        schemas[f"{model}List"] = {
            "type": "object",
            "properties": {
                "items": {
                    "type": "array",
                    "items": {"$ref": f"#/components/schemas/{model}"},
                },
                "total": {"type": "integer"},
                "page": {"type": "integer"},
            },
        }

    paths = {}
    for resource in ("users", "projects", "tasks"):
        paths[f"/api/v1/{resource}"] = {
            method: {
                "operationId": f"{method}_{resource}",
                "tags": [resource],
                "parameters": [
                    {"name": "page", "in": "query", "schema": {"type": "integer"}},
                    {"name": "per_page", "in": "query", "schema": {"type": "integer"}},
                ],
                "responses": {
                    "200": {"description": "OK"},
                    "404": {"description": "Not found"},
                },
            }
            for method in ("get", "post")
        }

    return {
        "openapi": "3.0.3",
        "info": {"title": "Example API", "version": "1.0.0"},
        "paths": paths,
        "components": {"schemas": schemas},
    }


def make_tabular_data(n):
    categories = ("A", "B", "C", "D")
    return [
        {
            "id": i,
            "name": f"item_{i}",
            "value": float(i * 3.14),
            "category": categories[i % 4],
            "active": i % 7 != 0,
            "tags": [f"t{j}" for j in range(i % 4)],
        }
        for i in range(n)
    ]


def make_grayscale_image_1024x1024():
    return [[(r * 4 + c) % 256 for c in range(1024)] for r in range(1024)]


@dataclass
class OrmUser:
    id: int
    name: str
    prefs: dict = field(default_factory=dict)
    sessions: list = field(default_factory=list)


@dataclass
class OrmSession:
    token: str
    created: datetime
    data: dict = field(default_factory=dict)


def make_orm_graph():
    shared_prefs = {"theme": "dark", "lang": "en", "notifications": True}
    return [
        OrmUser(
            i,
            f"u{i}",
            shared_prefs,
            [
                OrmSession(
                    f"t{i}{j}",
                    datetime(2024, 1, 1 + j),  # noqa: DTZ001
                    {"ip": f"10.0.{i}.{j}"},
                )
                for j in range(3)
            ],
        )
        for i in range(10)
    ]


REAL_WORLD_CASES = [
    Case("json_api_response", make_json_api_response()),
    Case("config_shared_defaults", make_config_with_shared_defaults()),
    Case("openapi_schema", make_openapi_fragment()),
    Case("tabular_100", make_tabular_data(100)),
    Case("tabular_1000", make_tabular_data(1000)),
    Case("image_1024x1024", make_grayscale_image_1024x1024()),
    Case("orm_graph_10u3s", make_orm_graph()),
]


# ═══════════════════════════════════════════════════════════
#  TESTS
# ═══════════════════════════════════════════════════════════


@generate_params(MEMO_CASES)
@PYTHON_VERSION
def test_memo(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(CONTAINER_CASES)
@PYTHON_VERSION
def test_container(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(DEPTH_CASES)
@PYTHON_VERSION
def test_depth(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(ATOMIC_CASES)
@PYTHON_VERSION
def test_atomic(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(REDUCE_CASES)
@PYTHON_VERSION
def test_reduce(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(EDGE_CASES)
@PYTHON_VERSION
def test_edge(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(REAL_WORLD_CASES)
@PYTHON_VERSION
def test_real(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj)


@generate_params(REAL_WORLD_CASES)
@PYTHON_VERSION
def test_real_dict_memo(case: Case, _python, benchmark):
    benchmark(copium.deepcopy, case.obj, {})


@generate_params(REAL_WORLD_CASES)
@PYTHON_VERSION
def test_real_stdlib_patched(case: Case, _python, benchmark, copium_patch_enabled):
    benchmark(stdlib_copy.deepcopy, case.obj)


@generate_params(REAL_WORLD_CASES)
@PYTHON_VERSION
def test_real_stdlib(case: Case, _python, benchmark):
    benchmark(stdlib_copy.deepcopy, case.obj)
