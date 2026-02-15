#!/usr/bin/env python3
"""Generate benchmark chart SVG using declarative DSL."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from tools.terminal_svg import DARK, LIGHT, In, IPython, TerminalWindow, TimeitBar


DEFAULT_NOTEBOOK = Path("showcase.ipynb")


def create_chart(
    stdlib_seconds: float,
    copium_seconds: float,
) -> TerminalWindow:
    return TerminalWindow(
        IPython(
            In("from jsonschema import Draft202012Validator"),
            In("from copy import deepcopy"),
            In("%timeit deepcopy(Draft202012Validator.META_SCHEMA)"),
            (baseline := TimeitBar(stdlib_seconds, style="slow")),
            In("import copium.patch; copium.patch.enable();"),
            In("%timeit deepcopy(Draft202012Validator.META_SCHEMA)"),
            TimeitBar(copium_seconds, style="fast", baseline=baseline),
        ),
        title="uvx --with copium --with jsonschema ipython",
        min_width=600
    )


def extract_median_from_pyperf(path: Path) -> float:
    data = json.loads(path.read_text())
    runs = data["benchmarks"][0]["runs"]
    values = [v for r in runs for v in r.get("values", [])]
    return sorted(values)[len(values) // 2]


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate benchmark chart SVG")

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--times", nargs=2, type=float, metavar=("STDLIB", "COPIUM"),
                       help="Times in seconds: stdlib copium")
    group.add_argument("--pyperf", nargs=2, type=Path, metavar=("STDLIB_JSON", "COPIUM_JSON"),
                       help="Pyperf result JSON files: stdlib.json copium.json")

    parser.add_argument("--notebook", type=Path, default=DEFAULT_NOTEBOOK,
                        help="Notebook to extract data expression from")
    parser.add_argument("--theme", choices=["dark", "light", "both"], default="both")
    parser.add_argument("--output-dir", "-o", type=Path, default=Path("assets"))

    args = parser.parse_args()

    if args.times:
        stdlib_time, copium_time = args.times
    else:
        stdlib_time = extract_median_from_pyperf(args.pyperf[0])
        copium_time = extract_median_from_pyperf(args.pyperf[1])

    chart = create_chart(stdlib_time, copium_time)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    themes = []
    if args.theme in ("dark", "both"):
        themes.append(("dark", DARK))
    if args.theme in ("light", "both"):
        themes.append(("light", LIGHT))

    for name, theme in themes:
        output_path = args.output_dir / f"chart_{name}.svg"
        chart.save(str(output_path), theme)
        print(f"Generated {output_path}")

    speedup = stdlib_time / copium_time
    print(f"Speedup: {speedup:.1f}×")


if __name__ == "__main__":
    main()
