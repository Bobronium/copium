#!/usr/bin/env python3
"""
Run benchmark notebook, generate charts from results.

Usage:
    python -m tools.benchmark_chart --runs 5 --output-dir assets
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import median
from tempfile import TemporaryDirectory


RESULTS_PATH = Path(".bench/chart_results.json")


@dataclass
class BenchmarkResult:
    run_id: int
    stdlib_average: float
    stdlib_stdev: float
    copium_average: float
    copium_stdev: float
    notebook_path: Path
    results_path: Path

    @property
    def speedup(self) -> float:
        return self.stdlib_average / self.copium_average


def run_notebook(notebook_src: Path, work_dir: Path, run_id: int) -> BenchmarkResult | None:
    notebook_copy = work_dir / f"run_{run_id}.ipynb"
    results_copy = work_dir / f"results_{run_id}.json"
    
    shutil.copy(notebook_src, notebook_copy)
    
    try:
        subprocess.run(
            [sys.executable, "-m", "jupytext", "--execute", "--update", str(notebook_copy)],
            check=True,
            capture_output=True,
            cwd=work_dir,
        )
    except subprocess.CalledProcessError as e:
        print(f"FAILED: {e.stderr.decode()[:200]}")
        return None
    
    results_in_workdir = work_dir / ".bench" / "chart_results.json"
    if not results_in_workdir.exists():
        print("FAILED: notebook didn't produce results JSON")
        return None
    
    shutil.move(results_in_workdir, results_copy)
    
    data = json.loads(results_copy.read_text())
    
    return BenchmarkResult(
        run_id=run_id,
        stdlib_average=data["stdlib"]["average"],
        stdlib_stdev=data["stdlib"]["stdev"],
        copium_average=data["copium"]["average"],
        copium_stdev=data["copium"]["stdev"],
        notebook_path=notebook_copy,
        results_path=results_copy,
    )


def run_benchmarks(notebook_src: Path, num_runs: int, work_dir: Path) -> list[BenchmarkResult]:
    results = []
    
    for i in range(num_runs):
        print(f"Run {i + 1}/{num_runs}...", end=" ", flush=True)
        
        result = run_notebook(notebook_src, work_dir, i)
        if result:
            print(f"stdlib={result.stdlib_average:.3f}s, copium={result.copium_average:.4f}s, speedup={result.speedup:.1f}×")
            results.append(result)
    
    return results


def select_best_run(results: list[BenchmarkResult]) -> BenchmarkResult:
    copium_times = [r.copium_average for r in results]
    target = median(copium_times)
    return min(results, key=lambda r: abs(r.copium_average - target))


def generate_charts(
    stdlib_time: float,
    copium_time: float,
    output_dir: Path,
    notebook_path: Path,
) -> None:
    from tools.generate_chart import create_chart
    from tools.terminal_svg import DARK, LIGHT
    
    chart = create_chart(stdlib_time, copium_time, notebook_path=notebook_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    chart.save(str(output_dir / "chart_dark.svg"), DARK)
    chart.save(str(output_dir / "chart_light.svg"), LIGHT)
    
    print(f"Generated charts in {output_dir}/")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run benchmarks and generate charts")
    parser.add_argument("--notebook", default="showcase.ipynb")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--output-dir", default="assets")
    parser.add_argument("--save-notebook", metavar="PATH")
    parser.add_argument("--save-results", metavar="PATH")
    
    args = parser.parse_args()
    notebook_src = Path(args.notebook)
    
    with TemporaryDirectory(prefix="copium_bench_") as tmpdir:
        work_dir = Path(tmpdir)
        
        results = run_benchmarks(notebook_src, args.runs, work_dir)
        if not results:
            print("No successful runs!", file=sys.stderr)
            sys.exit(1)
        
        best = select_best_run(results)
        print(f"\nSelected run {best.run_id}: speedup={best.speedup:.1f}×")
        
        generate_charts(best.stdlib_average, best.copium_average, Path(args.output_dir), notebook_src)
        
        if args.save_notebook:
            shutil.copy(best.notebook_path, args.save_notebook)
            print(f"Saved notebook to {args.save_notebook}")
        
        if args.save_results:
            shutil.copy(best.results_path, args.save_results)
            print(f"Saved results to {args.save_results}")


if __name__ == "__main__":
    main()
