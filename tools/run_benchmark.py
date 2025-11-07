import copy
import os
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any
from typing import Dict, Optional, Tuple

import pyperf


if os.getenv("BENCHMARK_FOR_CHART"):
    # raise absolute case time up to perceivable value
    # and align stdlib bars for both cases
    # we can do it since we're interested in speedup ratio, not absolute values
    # and the reason is: it looks nicer.
    MIXED_MULTIPLIER = 20000
    BUILTIN_MULTIPLIER = 29000
else:
    MIXED_MULTIPLIER = BUILTIN_MULTIPLIER = 1


@dataclass
class User:
    id: int
    username: str
    email: str
    metadata: Dict[str, any] = field(default_factory=dict)
    tags: Tuple[str, ...] = ()
    preferences: Optional[dict] = None


@dataclass
class CacheEntry:
    key: str
    value: bytes
    hits: int
    created_at: datetime


def get_data(new_datetime, new_user, new_cache_entry) -> dict[str, Any]:
    # shared refs
    common_tags = ("premium", "verified", "active")
    default_settings = {"theme": "dark", "notifications": True, "language": "en"}

    return {
        "pending_tasks": [],
        "failed_operations": set(),
        "archived_sessions": {},
        "version": 3,
        "instance_id": "prod-api-01",
        "secret_key": b"\x89PNG\r\n\x1a\n\x00\x00",
        "maintenance_mode": None,
        "active_user_ids": [1001, 1002, 1003, 1001],
        "allowed_ips": frozenset(["127.0.0.1", "192.168.1.1", "10.0.0.1"]),
        "error_codes": (400, 401, 403, 404, 500),
        "default_settings": default_settings,
        "fallback_settings": default_settings,
        "users": {
            1001: new_user(
                id=1001,
                username="alice",
                email="alice@example.com",
                metadata={"joined": "2023-01-15", "level": 5},
                tags=common_tags,
                preferences=default_settings,
            ),
            1002: new_user(
                id=1002,
                username="bob",
                email="bob@example.com",
                metadata={},
                tags=common_tags,
                preferences=None,
            ),
        },
        "cache": {
            "user_data": [
                new_cache_entry("user:1001", b"serialized_data_1", 42, new_datetime()),
                new_cache_entry("user:1002", b"serialized_data_2", 15, new_datetime()),
            ],
            "session_data": [
                new_cache_entry("sess:abc", b"session_bytes", 100, new_datetime()),
            ],
            "stats": {
                "total_entries": 3,
                "memory_usage_bytes": 2048,
                "last_cleanup": new_datetime(),
            },
        },
        "feature_flags": {
            "authentication": {
                "oauth": {
                    "providers": {
                        "google": {"enabled": True, "client_id": "xxx"},
                        "github": {"enabled": False, "client_id": None},
                    }
                },
                "mfa": {
                    "methods": ["totp", "sms", "email"],
                    "required_for": frozenset(["admin", "premium"]),
                },
            },
            "api": {
                "rate_limits": {
                    "anonymous": (100, 3600),
                    "authenticated": (1000, 3600),
                    "premium": (10000, 3600),
                }
            },
        },
        "recent_snapshots": (
            {"timestamp": new_datetime(), "user_count": 150},
            {"timestamp": new_datetime(), "user_count": 148},
            {},
        ),
        "notification_queue": [
            {"type": "email", "template": default_settings},
            {"type": "push", "template": default_settings},
            {"type": "sms", "template": {"message": "test"}},
        ],
    }


def benchmark_mixed(n):
    """
    Run benchmark on types present in builtins,
    as well as dataclasses and datetime to account for reduce protocol.
    """
    total = 0
    value = get_data(lambda: datetime.fromtimestamp(123456789), User, CacheEntry)
    for ii in range(n * MIXED_MULTIPLIER):
        t0 = pyperf.perf_counter()
        copy.deepcopy(value)
        total += pyperf.perf_counter() - t0
    return total


def benchmark_builtins(n):
    """
    Run benchmark on types present in builtins.
    """
    value = get_data(lambda: 123456789, dict, lambda *args: tuple(args))
    total = 0
    for _ in range(n * BUILTIN_MULTIPLIER):
        t0 = pyperf.perf_counter()
        copy.deepcopy(value)
        total += pyperf.perf_counter() - t0
    return total


if __name__ == "__main__":
    runner = pyperf.Runner()
    runner.metadata["implementation"] = "copium" if os.getenv("COPIUM_PATCH_DEEPCOPY") else "copy"
    runner.bench_time_func("mixed", benchmark_mixed, metadata={"name": "mixed"})
    runner.bench_time_func("builtin", benchmark_builtins)
