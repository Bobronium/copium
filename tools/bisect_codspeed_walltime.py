from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

TEST_FILE = "tests/test_performance.py"
TEST_K = "test_performance"

PYTEST_BASE = [
    "uv",
    "run",
    "--no-sync",
    "pytest",
    TEST_FILE,
    "--codspeed",
    "-k",
    TEST_K,
    "--codspeed-warmup-time=0",
    "--codspeed-max-time=0.001",
    "--codspeed-max-rounds=1",
    "-q",
    "-p",
    "no:random-order",
    "--override-ini=addopts=",
]

CRASH_CODES = {139, -11}
CACHE_FILE = Path(".ci-codspeed-bisect-cache.json")
OUT_DIR = Path(".ci-codspeed-bisect")
OUT_DIR.mkdir(exist_ok=True)


@dataclass(frozen=True)
class RunResult:
    code: int
    crashed: bool
    stdout_tail: str
    stderr_tail: str


def sh(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        text=True,
        capture_output=True,
        check=False,
        env=os.environ.copy(),
    )


def collect_nodeids() -> list[str]:
    cmd = [
        "uv",
        "run",
        "--no-sync",
        "pytest",
        TEST_FILE,
        "-k",
        TEST_K,
        "--collect-only",
        "-q",
        "-p",
        "no:random-order",
        "--override-ini=addopts=",
    ]
    proc = sh(cmd)
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(f"collect failed: exit={proc.returncode}")

    nodeids: list[str] = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("tests/") and "::" in line:
            nodeids.append(line)

    if not nodeids:
        raise SystemExit("no nodeids collected")

    return nodeids


def load_cache() -> dict[str, dict]:
    if CACHE_FILE.exists():
        return json.loads(CACHE_FILE.read_text())
    return {}


def save_cache(cache: dict[str, dict]) -> None:
    CACHE_FILE.write_text(json.dumps(cache, indent=2, sort_keys=True))


def key_for_subset(nodeids: Iterable[str]) -> str:
    return "\n".join(nodeids)


def write_nodeids_file(path: Path, nodeids: list[str]) -> None:
    path.write_text("".join(f"{x}\n" for x in nodeids))


def run_subset(nodeids: list[str], *, label: str, use_cache: bool = True) -> RunResult:
    cache = load_cache()
    key = key_for_subset(nodeids)
    if use_cache and key in cache:
        entry = cache[key]
        return RunResult(
            code=entry["code"],
            crashed=entry["crashed"],
            stdout_tail=entry["stdout_tail"],
            stderr_tail=entry["stderr_tail"],
        )

    listfile = OUT_DIR / f"{label.replace('/', '_').replace(' ', '_')}.txt"
    write_nodeids_file(listfile, nodeids)

    cmd = PYTEST_BASE + [f"@{listfile}"]

    print(f"\n=== RUN {label} ===")
    print(f"tests: {len(nodeids)}")
    print("cmd:", shlex.join(cmd))

    proc = sh(cmd)

    stdout_tail = "\n".join(proc.stdout.splitlines()[-40:])
    stderr_tail = "\n".join(proc.stderr.splitlines()[-40:])
    crashed = proc.returncode in CRASH_CODES

    result = RunResult(
        code=proc.returncode,
        crashed=crashed,
        stdout_tail=stdout_tail,
        stderr_tail=stderr_tail,
    )

    if use_cache:
        cache[key] = {
            "code": result.code,
            "crashed": result.crashed,
            "stdout_tail": result.stdout_tail,
            "stderr_tail": result.stderr_tail,
        }
        save_cache(cache)

    print(f"exit={result.code} crashed={result.crashed}")
    if result.stdout_tail:
        print("--- stdout tail ---")
        print(result.stdout_tail)
    if result.stderr_tail:
        print("--- stderr tail ---", file=sys.stderr)
        print(result.stderr_tail, file=sys.stderr)

    return result


def partitions(xs: list[str], n: int) -> list[list[str]]:
    size = len(xs)
    out: list[list[str]] = []
    start = 0
    for i in range(n):
        end = start + (size - start + (n - i) - 1) // (n - i)
        out.append(xs[start:end])
        start = end
    return [p for p in out if p]


def without(xs: list[str], ys: Iterable[str]) -> list[str]:
    ys_set = set(ys)
    return [x for x in xs if x not in ys_set]


def ddmin(nodeids: list[str]) -> list[str]:
    current = nodeids[:]
    n = 2

    while len(current) >= 2:
        chunks = partitions(current, n)
        reduced = False

        for i, chunk in enumerate(chunks, 1):
            res = run_subset(chunk, label=f"ddmin_subset_{len(current)}_{i}_of_{len(chunks)}")
            if res.crashed:
                current = chunk
                n = 2
                reduced = True
                print(f"reduced to crashing subset of {len(current)} tests")
                break
        if reduced:
            continue

        for i, chunk in enumerate(chunks, 1):
            comp = without(current, chunk)
            if not comp:
                continue
            res = run_subset(comp, label=f"ddmin_complement_{len(current)}_{i}_of_{len(chunks)}")
            if res.crashed:
                current = comp
                n = max(n - 1, 2)
                reduced = True
                print(f"reduced to crashing complement of {len(current)} tests")
                break
        if reduced:
            continue

        if n >= len(current):
            break
        n = min(len(current), n * 2)

    return current


def find_last_passing_prefix(nodeids: list[str]) -> int:
    lo = 0
    hi = len(nodeids)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        res = run_subset(nodeids[:mid], label=f"prefix_0_{mid}")
        if res.crashed:
            hi = mid - 1
        else:
            lo = mid
    return lo


def verify_isolated_group(group: list[str], idx: int) -> None:
    res = run_subset(group, label=f"verify_group_{idx}")
    if not res.crashed:
        raise SystemExit(
            f"group {idx} is not independently crashing; "
            "this means the crash depends on interaction with outside tests"
        )


def verify_remainder_passes(remainder: list[str]) -> None:
    res = run_subset(remainder, label="verify_remainder")
    if res.crashed:
        raise SystemExit(
            "suite still crashes after excluding all discovered failing groups; "
            "partition is incomplete"
        )


def verify_each_group_against_clean_remainder(remainder: list[str], groups: list[list[str]]) -> None:
    for i, group in enumerate(groups, 1):
        combo = remainder + group
        res = run_subset(combo, label=f"verify_remainder_plus_group_{i}")
        if not res.crashed:
            raise SystemExit(
                f"clean remainder + failing group {i} did not crash; "
                "group is not sufficient in the final partition"
            )


def main() -> int:
    all_nodeids = collect_nodeids()
    print(f"collected {len(all_nodeids)} nodeids")

    full = run_subset(all_nodeids, label="full_suite", use_cache=False)
    if not full.crashed:
        print("full suite did not reproduce crash")
        return 2

    last_ok = find_last_passing_prefix(all_nodeids)
    print(f"last passing prefix length: {last_ok}")
    if last_ok < len(all_nodeids):
        print("first suspect after passing prefix:")
        print(all_nodeids[last_ok])

    remaining = all_nodeids[:]
    failing_groups: list[list[str]] = []

    iteration = 0
    while True:
        iteration += 1
        res = run_subset(remaining, label=f"remaining_{iteration}", use_cache=False)
        if not res.crashed:
            break

        print(f"\n### extracting crashing group #{iteration} from {len(remaining)} tests")
        group = ddmin(remaining)

        group_path = OUT_DIR / f"failing_group_{iteration}.txt"
        write_nodeids_file(group_path, group)
        print(f"wrote {group_path}")

        verify_isolated_group(group, iteration)

        new_remaining = without(remaining, group)
        remaining_res = run_subset(new_remaining, label=f"remaining_after_group_{iteration}", use_cache=False)

        failing_groups.append(group)
        remaining = new_remaining

        print(
            f"group #{iteration}: {len(group)} tests; "
            f"remaining: {len(remaining)}; "
            f"remaining_crashes={remaining_res.crashed}"
        )

        if not remaining_res.crashed:
            break

    passing_tests = remaining[:]

    print("\n=== FINAL PARTITION ===")
    print(f"failing groups: {len(failing_groups)}")
    for i, group in enumerate(failing_groups, 1):
        print(f"  group {i}: {len(group)} tests")
        for t in group:
            print(f"    {t}")
    print(f"passing remainder: {len(passing_tests)} tests")

    verify_remainder_passes(passing_tests)
    verify_each_group_against_clean_remainder(passing_tests, failing_groups)

    write_nodeids_file(OUT_DIR / "passing_tests.txt", passing_tests)
    for i, group in enumerate(failing_groups, 1):
        write_nodeids_file(OUT_DIR / f"failing_group_{i}.txt", group)

    summary = {
        "total_tests": len(all_nodeids),
        "failing_group_count": len(failing_groups),
        "failing_groups": failing_groups,
        "passing_tests": passing_tests,
    }
    (OUT_DIR / "summary.json").write_text(json.dumps(summary, indent=2))

    print("\npartition complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())