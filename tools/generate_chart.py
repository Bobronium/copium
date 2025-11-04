from __future__ import annotations

import colorsys
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Literal, Sequence

import matplotlib.colors as mcolors
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.path import Path as MplPath


# ============================================================================
# Data Shapes
# ============================================================================


@dataclass(frozen=True)
class PerfPoint:
    """Single performance measurement."""

    name: str
    seconds: float


@dataclass(frozen=True)
class PerfGroup:
    """Group of related performance measurements."""

    points: tuple[PerfPoint, ...]

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(p.name for p in self.points)

    @property
    def values(self) -> tuple[float, ...]:
        return tuple(p.seconds for p in self.points)


@dataclass(frozen=True)
class Theme:
    """Visual theme configuration."""

    name: Literal["dark", "light"]
    text_color: str
    grid_color: tuple[float, float, float, float]
    display_background: tuple[float, float, float, float]

    @staticmethod
    def dark() -> Theme:
        return Theme(
            name="dark",
            text_color="#C9D1D9",
            grid_color=(0.498, 0.498, 0.498, 0.25),
            display_background=(0.0, 0.0, 0.0, 1.0),
        )

    @staticmethod
    def light() -> Theme:
        return Theme(
            name="light",
            text_color="#333333",
            grid_color=(0.498, 0.498, 0.498, 0.25),
            display_background=(1.0, 1.0, 1.0, 1.0),
        )


@dataclass(frozen=True)
class ColorPalette:
    """Color endpoints for performance visualization."""

    fast: str
    slow: str


@dataclass(frozen=True)
class ColorMapping:
    """Parameters controlling time-to-color transformation."""

    slope: float = 1.6
    midpoint: float = 0.55
    sat_scale: float = 1.0
    val_scale: float = 1.0


@dataclass(frozen=True)
class TimeScale:
    """How to map time values to color positions."""

    mode: Literal["ratio", "linear", "anchored"] = "ratio"
    multiplier: float = 1.0
    green_threshold: float | None = None  # for 'anchored' mode
    red_threshold: float | None = None  # for 'anchored' mode


@dataclass(frozen=True)
class GradientConfig:
    """Global gradient rendering configuration."""

    enabled: bool = True
    resolution: int = 1024
    tilt: float = 0.0  # per-bar midpoint adjustment


@dataclass(frozen=True)
class ChartGeometry:
    """Physical dimensions of the chart."""

    width: float = 10.0
    row_height: float = 0.58
    bar_height: float = 0.55


@dataclass(frozen=True)
class SortSpec:
    """How to order items in display."""

    order: Literal["asc", "desc", "input"] = "asc"


@dataclass(frozen=True)
class GroupSortSpec:
    """How to order groups and items within groups."""

    metric: Literal["min", "max", "mean", "geomean"] = "min"
    group_order: Literal["asc", "desc"] = "asc"
    within_order: Literal["asc", "desc"] = "asc"


@dataclass(frozen=True)
class ChartConfig:
    """Complete chart rendering configuration."""

    theme: Theme
    palette: ColorPalette
    color_mapping: ColorMapping
    time_scale: TimeScale
    gradient: GradientConfig
    geometry: ChartGeometry


@dataclass(frozen=True)
class GroupChartConfig(ChartConfig):
    """Configuration with group-specific parameters."""

    gap_rows: int = 1


@dataclass(frozen=True)
class AxisTicks:
    """Computed axis tick positions and labels."""

    positions: tuple[float, ...]
    labels: tuple[str, ...]
    max_value: float


@dataclass(frozen=True)
class FileSpec:
    """Specification for loading performance data from file."""

    path: Path
    label_template: str  # may contain {benchname}


@dataclass(frozen=True)
class BenchmarkData:
    """Loaded benchmark measurements."""

    name: str
    values: tuple[float, ...]


# ============================================================================
# Pure Functions: Time Formatting
# ============================================================================


def format_time(seconds: float) -> str:
    """Format time value with appropriate units."""
    if seconds < 1e-6:
        ns = seconds * 1e9
        return f"{ns:.2f}ns" if ns < 10 else f"{ns:.1f}ns" if ns < 100 else f"{ns:.0f}ns"
    if seconds < 1e-3:
        us = seconds * 1e6
        return f"{us:.2f}µs" if us < 10 else f"{us:.1f}µs" if us < 100 else f"{us:.0f}µs"
    if seconds < 1:
        ms = seconds * 1e3
        return f"{ms:.2f}ms" if ms < 10 else f"{ms:.1f}ms" if ms < 100 else f"{ms:.0f}ms"
    return (
        f"{seconds:.2f}s"
        if seconds < 10
        else f"{seconds:.1f}s"
        if seconds < 100
        else f"{seconds:.0f}s"
    )


def compute_ticks(max_val: float, pad_ratio: float = 0.04, target_ticks: int = 3) -> AxisTicks:
    """Compute nice axis tick positions."""
    if max_val <= 0:
        return AxisTicks((0.0, 1.0), ("0s", "1s"), 1.0)

    def nice_step(val: float) -> float:
        raw = val / max(target_ticks, 1)
        magnitude = 10 ** math.floor(math.log10(raw)) if raw > 0 else 1
        residual = raw / magnitude
        nice = 1 if residual <= 1 else 2 if residual <= 2 else 5 if residual <= 5 else 10
        return nice * magnitude

    step = nice_step(max_val)
    n = int(math.ceil((max_val * (1 + pad_ratio)) / step))
    positions = tuple(i * step for i in range(n + 1))
    labels = tuple(format_time(v) for v in positions)
    return AxisTicks(positions, labels, max(positions))


# ============================================================================
# Pure Functions: Color Transformations
# ============================================================================


def scale_hsv(hex_color: str, sat_scale: float, val_scale: float) -> tuple[float, float, float]:
    """Scale saturation and value of a color in HSV space."""
    r, g, b = mcolors.to_rgb(hex_color)
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    s = np.clip(s * sat_scale, 0, 1)
    v = np.clip(v * val_scale, 0, 1)
    return colorsys.hsv_to_rgb(h, s, v)


def sigmoid(z: float) -> float:
    """Standard sigmoid function."""
    return 1.0 / (1.0 + math.exp(-z))


def logit(x: float, eps: float = 1e-12) -> float:
    """Inverse sigmoid (logit) function."""
    x = np.clip(x, eps, 1 - eps)
    return math.log(x / (1 - x))


def apply_sigmoid_bias(t: float, slope: float, midpoint: float) -> float:
    """Apply sigmoid transformation for edge compression."""
    z = slope * (logit(t) - logit(midpoint))
    return sigmoid(z)


def apply_sigmoid_bias_array(t: np.ndarray, slope: float, midpoint: float) -> np.ndarray:
    """Vectorized sigmoid bias application."""
    eps = 1e-12
    t_clipped = np.clip(t, eps, 1 - eps)
    z = slope * (np.log(t_clipped / (1 - t_clipped)) - logit(midpoint))
    return 1.0 / (1.0 + np.exp(-z))


def map_times_to_positions(
    values: Sequence[float],
    scale: TimeScale,
) -> tuple[float, ...]:
    """Map time values to [0,1] positions before sigmoid shaping."""
    if not values:
        return ()

    scaled = tuple(max(0.0, v) * scale.multiplier for v in values)
    vmin, vmax = min(scaled), max(scaled)

    if scale.mode == "anchored":
        if scale.green_threshold is None or scale.red_threshold is None:
            raise ValueError("anchored mode requires green_threshold and red_threshold")
        if scale.red_threshold <= scale.green_threshold:
            raise ValueError("red_threshold must be > green_threshold")
        return tuple(
            np.clip(
                (v - scale.green_threshold) / (scale.red_threshold - scale.green_threshold), 0, 1
            )
            for v in scaled
        )

    if vmax == vmin:
        return tuple(0.5 for _ in scaled)

    if scale.mode == "linear":
        return tuple((v - vmin) / (vmax - vmin) for v in scaled)

    if scale.mode == "ratio":
        rmax = vmax / vmin
        if rmax == 1:
            return tuple(0.0 for _ in scaled)
        return tuple(((v / vmin) - 1.0) / (rmax - 1.0) for v in scaled)

    raise ValueError(f"Unknown scale mode: {scale.mode}")


# ============================================================================
# Pure Functions: Gradient Generation
# ============================================================================


def compute_gradient_field(
    x_max: float,
    config: GradientConfig,
    color_mapping: ColorMapping,
    time_scale: TimeScale,
    n_rows: int = 1,
    row_values: Sequence[float] | None = None,
) -> np.ndarray:
    """Generate gradient field as (n_rows, resolution) array."""
    N = max(2, config.resolution)
    xs = np.linspace(0.0, x_max, N)

    # Map x to t
    if (
        time_scale.mode == "anchored"
        and time_scale.green_threshold is not None
        and time_scale.red_threshold is not None
        and time_scale.red_threshold > time_scale.green_threshold
    ):
        t = (xs - time_scale.green_threshold) / (
            time_scale.red_threshold - time_scale.green_threshold
        )
        t = np.clip(t, 0, 1)
    else:
        t = xs / x_max if x_max > 0 else np.zeros_like(xs)

    # Apply sigmoid per row with optional tilt
    field = np.empty((n_rows, N), dtype=float)

    if row_values is None or abs(config.tilt) < 1e-9:
        # No tilt: all rows identical
        shaped = apply_sigmoid_bias_array(t, color_mapping.slope, color_mapping.midpoint)
        field[:] = shaped
    else:
        # Per-row midpoint adjustment
        if len(row_values) != n_rows:
            raise ValueError("row_values length must match n_rows")

        denom = x_max if x_max > 0 else 1.0
        for i, val in enumerate(row_values):
            norm = val / denom
            midpoint_i = np.clip(color_mapping.midpoint * config.tilt * norm, 1e-3, 1 - 1e-3)
            field[i, :] = apply_sigmoid_bias_array(t, color_mapping.slope, midpoint_i)

    return field


# ============================================================================
# Pure Functions: Data Loading & Aggregation
# ============================================================================


def load_benchmark_file(path: Path) -> tuple[BenchmarkData, ...]:
    """Load benchmark data from JSON file."""
    with path.open("r", encoding="utf-8") as f:
        obj = json.load(f)

    benchmarks_list = obj.get("benchmarks", [])

    # Handle single-case files
    fallback_name = None
    if len(benchmarks_list) == 1:
        fallback_name = obj.get("metadata", {}).get("name")

    results: list[BenchmarkData] = []

    for b in benchmarks_list:
        name = b.get("metadata", {}).get("name") or fallback_name
        if name is None:
            raise ValueError(f"No name found in benchmark: {b}")

        values: list[float] = []
        for run in b.get("runs", []):
            values.extend(float(x) for x in run.get("values", []) if x is not None)

        if values:
            results.append(BenchmarkData(name, tuple(values)))

    return tuple(results)


def aggregate_values(
    values: Sequence[float], mode: Literal["median", "mean", "geomean", "min"]
) -> float:
    """Aggregate multiple measurements into single value."""
    if not values:
        raise ValueError("No values to aggregate")

    if mode == "median":
        return float(statistics.median(values))
    if mode == "mean":
        return float(statistics.fmean(values))
    if mode == "geomean":
        eps = 1e-300
        log_sum = sum(math.log(max(v, eps)) for v in values)
        return math.exp(log_sum / len(values))
    if mode == "min":
        return float(min(values))

    raise ValueError(f"Unknown aggregate mode: {mode}")


def build_groups_from_files(
    specs: Sequence[FileSpec],
    aggregate: Literal["median", "mean", "geomean", "min"],
    require_all: bool = False,
) -> tuple[PerfGroup, ...]:
    """Build performance groups from file specifications."""
    # Load all files
    datasets: list[tuple[str, dict[str, tuple[float, ...]]]] = []
    for spec in specs:
        benchmarks = load_benchmark_file(spec.path)
        data_dict = {b.name: b.values for b in benchmarks}
        datasets.append((spec.label_template, data_dict))

    # Collect all benchmark names
    all_names = set()
    for _, data in datasets:
        all_names.update(data.keys())

    # Build groups
    groups: list[PerfGroup] = []
    for bench_name in sorted(all_names):
        points: list[PerfPoint] = []

        for template, data in datasets:
            values = data.get(bench_name)
            if values is None:
                if require_all:
                    points = []
                    break
                continue

            label = template.format(benchname=bench_name)
            aggregated = aggregate_values(values, aggregate)
            points.append(PerfPoint(label, aggregated))

        if points:
            groups.append(PerfGroup(tuple(points)))

    if not groups:
        raise RuntimeError("No groups built from provided specs")

    return tuple(groups)


# ============================================================================
# Pure Functions: Sorting
# ============================================================================


def sort_group(group: PerfGroup, spec: SortSpec) -> PerfGroup:
    """Sort points within a group."""
    if spec.order == "input":
        return group

    reverse = spec.order == "desc"
    sorted_points = sorted(group.points, key=lambda p: p.seconds, reverse=reverse)
    return PerfGroup(tuple(sorted_points))


def compute_group_metric(
    group: PerfGroup, metric: Literal["min", "max", "mean", "geomean"]
) -> float:
    """Compute aggregate metric for a group."""
    values = group.values

    if metric == "min":
        return min(values)
    if metric == "max":
        return max(values)
    if metric == "mean":
        return sum(values) / len(values)
    if metric == "geomean":
        eps = 1e-300
        log_sum = sum(math.log(max(v, eps)) for v in values)
        return math.exp(log_sum / len(values))

    raise ValueError(f"Unknown metric: {metric}")


def sort_groups(groups: Sequence[PerfGroup], spec: GroupSortSpec) -> tuple[PerfGroup, ...]:
    """Sort groups and their contents."""
    # Sort within groups
    sorted_within = tuple(sort_group(g, SortSpec(order=spec.within_order)) for g in groups)

    # Sort groups by metric
    keyed = [(compute_group_metric(g, spec.metric), i, g) for i, g in enumerate(sorted_within)]
    reverse = spec.group_order == "desc"
    keyed.sort(key=lambda x: x[0], reverse=reverse)

    return tuple(g for _, _, g in keyed)


# ============================================================================
# Rendering
# ============================================================================


def union_clip_path(bars) -> MplPath:
    """Create union clip path from bar rectangles."""
    verts: list[tuple[float, float]] = []
    codes: list[int] = []

    for bar in bars:
        x0, x1 = bar.get_x(), bar.get_x() + bar.get_width()
        y0, y1 = bar.get_y(), bar.get_y() + bar.get_height()
        verts.extend([(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)])
        codes.extend(
            [MplPath.MOVETO, MplPath.LINETO, MplPath.LINETO, MplPath.LINETO, MplPath.CLOSEPOLY]
        )

    return MplPath(verts, codes)


def render_chart(
    data: PerfGroup,
    config: ChartConfig,
    sort_spec: SortSpec,
    save_png: Path | None = None,
    save_svg: Path | None = None,
    show: bool = False,
) -> tuple:
    """Render single-series performance chart."""
    sorted_data = sort_group(data, sort_spec)
    names = sorted_data.names
    values = sorted_data.values

    # Setup figure
    height = max(2.0, len(names) * config.geometry.row_height)
    fig, ax = plt.subplots(figsize=(config.geometry.width, height))
    fig.patch.set_facecolor("none")
    ax.set_facecolor("none")

    # Create invisible bars
    bars = ax.barh(
        names,
        values,
        height=config.geometry.bar_height,
        color=(0, 0, 0, 0),
        edgecolor=(0, 0, 0, 0),
        linewidth=0,
        zorder=2,
    )

    # Axis configuration
    ticks = compute_ticks(max(values))
    ax.set_xlim(0, ticks.max_value)
    ax.set_xticks(ticks.positions)
    ax.set_xticklabels(ticks.labels, color=config.theme.text_color, fontsize=12)
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(names, color=config.theme.text_color, fontsize=12)
    ax.tick_params(axis="both", colors=config.theme.text_color)

    # Highlight fastest
    min_val = min(values)
    for label in ax.get_yticklabels():
        point = next(p for p in sorted_data.points if p.name == label.get_text())
        if point.seconds == min_val:
            label.set_size(11)
            label.set_weight("bold")

    ax.set_axisbelow(True)
    ax.grid(axis="x", color=config.theme.grid_color, linewidth=1)
    for spine in ax.spines.values():
        spine.set_visible(False)

    # Color mapping
    fast_rgb = scale_hsv(
        config.palette.fast, config.color_mapping.sat_scale, config.color_mapping.val_scale
    )
    slow_rgb = scale_hsv(
        config.palette.slow, config.color_mapping.sat_scale, config.color_mapping.val_scale
    )
    cmap = mcolors.LinearSegmentedColormap.from_list("perf", [fast_rgb, slow_rgb])

    if config.gradient.enabled:
        # Global gradient
        n_rows = len(names) if abs(config.gradient.tilt) > 1e-9 else 1
        row_vals = values if n_rows > 1 else None

        field = compute_gradient_field(
            ticks.max_value,
            config.gradient,
            config.color_mapping,
            config.time_scale,
            n_rows,
            row_vals,
        )

        im = ax.imshow(
            field,
            aspect="auto",
            extent=[0, ticks.max_value, -0.5, len(names) - 0.5],
            origin="lower",
            interpolation="nearest",
            cmap=cmap,
            zorder=2,
        )
        im.set_clip_path(union_clip_path(bars), transform=ax.transData)
    else:
        # Solid colors
        positions = map_times_to_positions(values, config.time_scale)
        shaped = tuple(
            apply_sigmoid_bias(t, config.color_mapping.slope, config.color_mapping.midpoint)
            for t in positions
        )

        for bar, t in zip(bars, shaped):
            color = tuple((1 - t) * f + t * s for f, s in zip(fast_rgb, slow_rgb)) + (1.0,)
            bar.set_facecolor(color)

    # Value labels
    for bar, val in zip(bars, values):
        is_fastest = val == min_val
        ax.text(
            bar.get_width() + 0.012 * ticks.max_value,
            bar.get_y() + bar.get_height() / 2,
            format_time(val),
            va="center",
            ha="left",
            color=config.theme.text_color,
            fontsize=11 if is_fastest else 12,
            weight="bold" if is_fastest else "normal",
            zorder=3,
        )

    ax.invert_yaxis()
    plt.tight_layout()

    # Save and show
    if save_png:
        plt.savefig(save_png, dpi=150, bbox_inches="tight", transparent=True)
    if save_svg:
        plt.savefig(save_svg, bbox_inches="tight", transparent=True)

    if show:
        orig_fig_fc = fig.get_facecolor()
        orig_ax_fc = ax.get_facecolor()
        try:
            fig.patch.set_facecolor(config.theme.display_background)
            ax.set_facecolor(config.theme.display_background)
            fig.canvas.draw_idle()
            plt.show()
        finally:
            fig.patch.set_facecolor(orig_fig_fc)
            ax.set_facecolor(orig_ax_fc)

    return fig, ax


def render_grouped_chart(
    groups: Sequence[PerfGroup],
    config: GroupChartConfig,
    sort_spec: GroupSortSpec,
    save_png: Path | None = None,
    save_svg: Path | None = None,
    show: bool = False,
) -> tuple:
    """Render grouped performance chart."""
    sorted_groups = sort_groups(groups, sort_spec)

    # Flatten with spacers
    flat_points: list[PerfPoint | None] = []  # None = spacer
    group_index: list[int | None] = []
    fastest_per_group: dict[int, str] = {}

    for gi, group in enumerate(sorted_groups):
        flat_points.extend(group.points)
        group_index.extend([gi] * len(group.points))

        min_val = min(group.values)
        fastest_name = next(p.name for p in group.points if p.seconds == min_val)
        fastest_per_group[gi] = fastest_name

        # Add spacers between groups
        if gi < len(sorted_groups) - 1:
            for _ in range(config.gap_rows):
                flat_points.append(None)
                group_index.append(None)

    # Extract display arrays
    names = tuple(p.name if p else "" for p in flat_points)
    values = tuple(p.seconds if p else 0.0 for p in flat_points)
    real_mask = tuple(p is not None for p in flat_points)
    real_values = tuple(v for v, m in zip(values, real_mask) if m)

    # Setup figure
    height = max(2.0, len(names) * config.geometry.row_height)
    fig, ax = plt.subplots(figsize=(config.geometry.width, height))
    fig.patch.set_facecolor("none")
    ax.set_facecolor("none")

    # Create bars
    bars = ax.barh(
        np.arange(len(names)),
        values,
        height=config.geometry.bar_height,
        color=(0, 0, 0, 0),
        edgecolor=(0, 0, 0, 0),
        linewidth=0,
        zorder=2,
    )

    # Axis configuration
    ticks = compute_ticks(max(real_values))
    ax.set_xlim(0, ticks.max_value)
    ax.set_xticks(ticks.positions)
    ax.set_xticklabels(ticks.labels, color=config.theme.text_color, fontsize=12)
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(names, color=config.theme.text_color, fontsize=12)
    ax.tick_params(axis="both", colors=config.theme.text_color)

    # Highlight fastest per group
    for yi, label in enumerate(ax.get_yticklabels()):
        gi = group_index[yi]
        if gi is None:
            label.set_text("")
            continue
        if label.get_text() == fastest_per_group[gi]:
            label.set_size(11)
            label.set_weight("bold")

    ax.set_axisbelow(True)
    ax.grid(axis="x", color=config.theme.grid_color, linewidth=1)
    for spine in ax.spines.values():
        spine.set_visible(False)

    # Color mapping
    fast_rgb = scale_hsv(
        config.palette.fast, config.color_mapping.sat_scale, config.color_mapping.val_scale
    )
    slow_rgb = scale_hsv(
        config.palette.slow, config.color_mapping.sat_scale, config.color_mapping.val_scale
    )
    cmap = mcolors.LinearSegmentedColormap.from_list("perf", [fast_rgb, slow_rgb])

    if config.gradient.enabled:
        n_rows = len(names) if abs(config.gradient.tilt) > 1e-9 else 1
        row_vals = values if n_rows > 1 else None

        field = compute_gradient_field(
            ticks.max_value,
            config.gradient,
            config.color_mapping,
            config.time_scale,
            n_rows,
            row_vals,
        )

        im = ax.imshow(
            field,
            aspect="auto",
            extent=[0, ticks.max_value, -0.5, len(names) - 0.5],
            origin="lower",
            interpolation="nearest",
            cmap=cmap,
            zorder=2,
        )
        real_bars = [b for b, m in zip(bars, real_mask) if m]
        im.set_clip_path(union_clip_path(real_bars), transform=ax.transData)
    else:
        positions = map_times_to_positions(real_values, config.time_scale)
        shaped = tuple(
            apply_sigmoid_bias(t, config.color_mapping.slope, config.color_mapping.midpoint)
            for t in positions
        )

        pos_iter = iter(shaped)
        for bar, is_real in zip(bars, real_mask):
            if not is_real:
                continue
            t = next(pos_iter)
            color = tuple((1 - t) * f + t * s for f, s in zip(fast_rgb, slow_rgb)) + (1.0,)
            bar.set_facecolor(color)

    # Value labels (highlight fastest per group)
    group_mins = {gi: min(g.values) for gi, g in enumerate(sorted_groups)}

    for bar, val, gi, is_real in zip(bars, values, group_index, real_mask):
        if not is_real or gi is None:
            continue

        is_fastest = val == group_mins[gi]
        ax.text(
            bar.get_width() + 0.012 * ticks.max_value,
            bar.get_y() + bar.get_height() / 2,
            format_time(val),
            va="center",
            ha="left",
            color=config.theme.text_color,
            fontsize=11 if is_fastest else 12,
            weight="bold" if is_fastest else "normal",
            zorder=3,
        )

    ax.invert_yaxis()
    plt.tight_layout()

    # Save and show
    if save_png:
        plt.savefig(save_png, dpi=150, bbox_inches="tight", transparent=True)
    if save_svg:
        plt.savefig(save_svg, bbox_inches="tight", transparent=True)

    if show:
        orig_fig_fc = fig.get_facecolor()
        orig_ax_fc = ax.get_facecolor()
        try:
            fig.patch.set_facecolor(config.theme.display_background)
            ax.set_facecolor(config.theme.display_background)
            fig.canvas.draw_idle()
            plt.show()
        finally:
            fig.patch.set_facecolor(orig_fig_fc)
            ax.set_facecolor(orig_ax_fc)

    return fig, ax


# ============================================================================
# CLI
# ============================================================================


def compute_anchor_thresholds(
    groups: Sequence[PerfGroup],
    strategy: Literal["default", "tight", "loose", "green_favor", "red_favor"] = "green_favor",
) -> tuple[float, float]:
    """Compute green/red anchor thresholds from data."""
    all_values = np.array([p.seconds for g in groups for p in g.points])
    vmin, vmax = float(all_values.min()), float(all_values.max())

    if strategy == "default":
        green = max(0.0, vmin * 1.05)
        red = max(green * 1.001, vmax * 0.95)
    elif strategy == "tight":
        green = max(0.0, vmin * 1.15)
        red = max(green * 1.001, vmax * 0.85)
    elif strategy == "loose":
        green = max(0.0, vmin * 0.95)
        red = max(green * 1.001, vmax * 1.05)
    elif strategy == "green_favor":
        green = max(0.0, vmin * 1.02)
        red = max(green * 1.001, vmax * 1.00)
    elif strategy == "red_favor":
        green = max(0.0, vmin * 1.10)
        red = max(green * 1.001, vmax * 0.90)
    else:
        raise ValueError(f"Unknown strategy: {strategy}")

    return green, red


def main() -> None:
    """CLI entrypoint."""
    import argparse

    parser = argparse.ArgumentParser(description="Render grouped performance charts")
    parser.add_argument(
        "--name",
        action="append",
        required=True,
        help='PATH:TEMPLATE, e.g. "copium.json:copium({benchname})". Specify multiple times.',
    )
    parser.add_argument(
        "--aggregate",
        default="median",
        choices=["median", "mean", "geomean", "min"],
        help="How to aggregate multiple measurements",
    )
    parser.add_argument(
        "--require-all",
        action="store_true",
        help="Only include benchmarks present in all files",
    )
    parser.add_argument("--theme", default="dark", choices=["dark", "light"])
    parser.add_argument(
        "--group-sort",
        default="min",
        choices=["min", "max", "mean", "geomean"],
        help="Metric for sorting groups",
    )
    parser.add_argument(
        "--group-order",
        default="asc",
        choices=["asc", "desc"],
        help="Order for groups",
    )
    parser.add_argument(
        "--within-order",
        default="asc",
        choices=["asc", "desc"],
        help="Order within groups",
    )
    parser.add_argument("--save-svg", type=Path, help="Output SVG file")
    parser.add_argument("--save-png", type=Path, help="Output PNG file")
    parser.add_argument("--show", action="store_true", help="Show chart")

    args = parser.parse_args()

    # Parse file specs
    specs = tuple(
        FileSpec(Path(part.split(":", 1)[0]), part.split(":", 1)[1]) for part in args.name
    )

    # Load and build groups
    groups = build_groups_from_files(specs, args.aggregate, args.require_all)

    # Compute anchors
    green_at, red_at = compute_anchor_thresholds(groups, "green_favor")

    # Build configuration
    theme = Theme.dark() if args.theme == "dark" else Theme.light()

    palette = ColorPalette(
        fast="#3573A7" if args.theme == "dark" else "#00b3e5",
        slow="#E89B14",
    )

    config = GroupChartConfig(
        theme=theme,
        palette=palette,
        color_mapping=ColorMapping(slope=1.6, midpoint=0.40, sat_scale=1.1, val_scale=0.95),
        time_scale=TimeScale(mode="anchored", green_threshold=green_at, red_threshold=red_at),
        gradient=GradientConfig(enabled=True, resolution=1024, tilt=2.0),
        geometry=ChartGeometry(width=10.0, row_height=0.58, bar_height=0.55),
        gap_rows=1,
    )

    sort_spec = GroupSortSpec(
        metric=args.group_sort,
        group_order=args.group_order,
        within_order=args.within_order,
    )

    # Render
    render_grouped_chart(
        groups,
        config,
        sort_spec,
        save_png=args.save_png,
        save_svg=args.save_svg,
        show=args.show,
    )


if __name__ == "__main__":
    main()
