# /// script
# requires-python = ">=3.14"
# dependencies = [
#     "matplotlib",
#     "numpy",
# ]
# ///

import json
import math
from pathlib import Path
from typing import List, Tuple, Optional

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
from matplotlib.path import Path as MplPath
from matplotlib.patches import PathPatch
import colorsys


def get_mean(bench: dict) -> float:
    all_values = []
    for run in bench.get('runs', []):
        all_values.extend(run.get('values', []))
    return sum(all_values) / len(all_values) if all_values else 0.0


def _format_time_s(seconds: float) -> str:
    if seconds < 1e-6:
        ns = seconds * 1e9
        return f"{ns:.2f}ns" if ns < 10 else f"{ns:.1f}ns" if ns < 100 else f"{ns:.0f}ns"
    elif seconds < 1e-3:
        us = seconds * 1e6
        return f"{us:.2f}µs" if us < 10 else f"{us:.1f}µs" if us < 100 else f"{us:.0f}µs"
    elif seconds < 1:
        ms = seconds * 1e3
        return f"{ms:.2f}ms" if ms < 10 else f"{ms:.1f}ms" if ms < 100 else f"{ms:.0f}ms"
    else:
        return f"{seconds:.2f}s" if seconds < 10 else f"{seconds:.1f}s" if seconds < 100 else f"{seconds:.0f}s"


def _format_speedup(v: float) -> str:
    return f"{v:.1f}x" if v < 10 else f"{v:.0f}x"


def _nice_step(max_val: float, target_ticks: int = 3) -> float:
    if max_val <= 0:
        return 1.0
    raw = max_val / max(target_ticks, 1)
    magnitude = 10 ** math.floor(math.log10(raw)) if raw > 0 else 1
    residual = raw / magnitude
    if residual <= 1:
        nice = 1
    elif residual <= 2:
        nice = 2
    elif residual <= 5:
        nice = 5
    else:
        nice = 10
    return nice * magnitude


def _build_ticks(max_val: float, pad_ratio: float = 0.04, target_ticks: int = 3, format_func=lambda v: f"{v}"):
    if max_val <= 0:
        return [0, 1], [format_func(0), format_func(1)], 1
    step = _nice_step(max_val, target_ticks=target_ticks)
    n = int(math.ceil((max_val * (1 + pad_ratio)) / step))
    locs = [i * step for i in range(n + 1)]
    labels = [format_func(v) for v in locs]
    return locs, labels, max(locs) if locs else step


def _get_theme_colors(theme: str = "dark"):
    theme = theme.lower()
    if theme == "dark":
        return {"text": "#C9D1D9", "grid": (0.498, 0.498, 0.498, 0.25)}
    elif theme == "light":
        return {"text": "#333333", "grid": (0.498, 0.498, 0.498, 0.25)}
    raise ValueError("theme must be 'dark' or 'light'")


def _get_theme_background_rgba(theme: str = "dark"):
    t = theme.lower()
    if t == "dark":
        return (0.0, 0.0, 0.0, 1.0)
    if t == "light":
        return (1.0, 1.0, 1.0, 1.0)
    raise ValueError("theme must be 'dark' or 'light'")


def _scale_hsv(hex_color: str, sat_scale: float = 1.0, val_scale: float = 1.0):
    r, g, b = mcolors.to_rgb(hex_color)
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    s = max(0.0, min(1.0, s * sat_scale))
    v = max(0.0, min(1.0, v * val_scale))
    r2, g2, b2 = colorsys.hsv_to_rgb(h, s, v)
    return (r2, g2, b2)


def _logit(x: float, eps: float = 1e-12) -> float:
    x = min(max(x, eps), 1 - eps)
    return math.log(x / (1 - x))


def _sigmoid(z: float) -> float:
    return 1.0 / (1.0 + math.exp(-z))


def _apply_sigmoid_bias(t: float, slope: float = 1.5, midpoint: float = 0.5) -> float:
    z = slope * (_logit(t) - _logit(midpoint))
    return _sigmoid(z)


def _apply_sigmoid_bias_vec(ts: np.ndarray, slope: float = 1.5, midpoint: float = 0.5) -> np.ndarray:
    eps = 1e-12
    ts = np.clip(ts, eps, 1 - eps)
    z = slope * (np.log(ts / (1 - ts)) - math.log(midpoint / (1 - midpoint)))
    return 1.0 / (1.0 + np.exp(-z))


def _positions_from_times(
    values: List[float],
    *,
    scale_mode: str = "ratio",
    time_scale: float = 1.0,
    green_at: Optional[float] = None,
    red_at: Optional[float] = None,
) -> List[float]:
    if not values:
        return []
    v = [max(0.0, float(x)) * time_scale for x in values]
    vmin, vmax = min(v), max(v)
    if scale_mode == "anchored":
        if green_at is None or red_at is None or red_at <= green_at:
            raise ValueError("For scale_mode='anchored', set green_at < red_at (seconds).")
        return [min(1.0, max(0.0, (x - green_at) / (red_at - green_at))) for x in v]
    if vmax == vmin:
        return [0.5] * len(v)
    if scale_mode == "linear":
        return [(x - vmin) / (vmax - vmin) for x in v]
    if scale_mode == "ratio":
        rmax = vmax / vmin
        if rmax == 1:
            return [0.0 for _ in v]
        return [((x / vmin) - 1.0) / (rmax - 1.0) for x in v]
    raise ValueError("scale_mode must be one of {'ratio','linear','anchored'}")


def _global_gradient_data(
    xmax: float,
    *,
    resolution: int = 1024,
    color_slope: float = 1.6,
    color_midpoint: float = 0.55,
    scale_mode: str = "linear",
    green_at: Optional[float] = None,
    red_at: Optional[float] = None,
    n_rows: int = 1,
    row_midpoints: Optional[Sequence[float]] = None,
):
    N = max(2, int(resolution))
    xs = np.linspace(0.0, xmax, N)
    if scale_mode == "anchored" and green_at is not None and red_at is not None and red_at > green_at:
        t = (xs - green_at) / (red_at - green_at)
        t = np.clip(t, 0.0, 1.0)
    else:
        t = xs / xmax if xmax > 0 else np.zeros_like(xs)
    shaped = np.empty((n_rows, N), dtype=float)
    if row_midpoints is None:
        shaped[:] = _apply_sigmoid_bias_vec(t, slope=color_slope, midpoint=color_midpoint)
    else:
        if len(row_midpoints) != n_rows:
            raise ValueError("row_midpoints length must match n_rows")
        for i, mid in enumerate(row_midpoints):
            mid = float(np.clip(mid, 1e-6, 1 - 1e-6))
            shaped[i, :] = _apply_sigmoid_bias_vec(t, slope=color_slope, midpoint=mid)
    return shaped


def _union_clip_path_from_bars(bars) -> MplPath:
    verts: List[Tuple[float, float]] = []
    codes: List[int] = []
    for b in bars:
        x0 = b.get_x()
        x1 = x0 + b.get_width()
        y0 = b.get_y()
        y1 = y0 + b.get_height()
        verts += [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
        codes += [MplPath.MOVETO, MplPath.LINETO, MplPath.LINETO, MplPath.LINETO, MplPath.CLOSEPOLY]
    return MplPath(verts, codes)


def render_perf_chart(
    data: List[Tuple[str, float]],
    *,
    order: str = "asc",
    theme: str = "dark",
    figsize_base_width: float = 10.0,
    row_height: float = 0.58,
    bar_height: float = 0.55,
    color_slope: float = 1.6,
    color_midpoint: float = 0.55,
    sat_scale: float = 1.0,
    val_scale: float = 1.0,
    scale_mode: str = "ratio",
    time_scale: float = 1.0,
    green_at: Optional[float] = None,
    red_at: Optional[float] = None,
    global_gradient: bool = True,
    gradient_resolution: int = 1024,
    gradient_tilt: float = 0.0,
    green: str = "#22C55E",
    red: str = "#EF4444",
    save_png: Optional[str] = None,
    save_svg: Optional[str | Path] = None,
    show: bool = True,
    debug_colors: bool = False,
    format_func = _format_time_s,
):
    colors_theme = _get_theme_colors(theme)
    items = [(str(n), float(t)) for n, t in data]
    if not items:
        raise ValueError("No data provided.")
    if order == "desc":
        items.sort(key=lambda x: x[1], reverse=True)
    elif order == "asc":
        items.sort(key=lambda x: x[1])
    elif order != "input":
        raise ValueError("order must be one of {'desc','asc','input'}")
    names = [n for n, _ in items]
    values = [t for _, t in items]
    h = max(2.0, len(items) * row_height)
    fig, ax = plt.subplots(figsize=(figsize_base_width, h))
    fig.patch.set_facecolor("none")
    ax.set_facecolor("none")
    bars = ax.barh(names, values, height=bar_height, color=(0, 0, 0, 0), edgecolor=(0, 0, 0, 0), linewidth=0.0, zorder=2.0)
    max_val = max(values)
    xticks, xticklabels, xmax = _build_ticks(max_val, pad_ratio=0.06, target_ticks=3, format_func=format_func)
    ax.set_xlim(0, xmax)
    ax.set_xticks(xticks)
    ax.set_xticklabels(xticklabels, color=colors_theme["text"], fontsize=12)
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(names, color=colors_theme["text"], fontsize=12)
    min_val = min(values)
    fastest_names = {n for (n, v) in items if v == min_val}
    for lbl in ax.get_yticklabels():
        if lbl.get_text() in fastest_names:
            lbl.set_size(11)
            lbl.set_weight("bold")
    ax.set_axisbelow(True)
    ax.grid(axis="x", color=colors_theme["grid"], linewidth=1, linestyle="-")
    for spine in ax.spines.values():
        spine.set_visible(False)
    g_rgb = _scale_hsv(green, sat_scale=sat_scale, val_scale=val_scale)
    r_rgb = _scale_hsv(red, sat_scale=sat_scale, val_scale=val_scale)
    cmap = mcolors.LinearSegmentedColormap.from_list("g2r", [g_rgb, r_rgb])
    if global_gradient:
        use_rows = len(names) if abs(gradient_tilt) > 0 else 1
        row_mids = None
        if use_rows > 1:
            denom = xmax if xmax > 0 else 1.0
            norms = np.array(values, dtype=float) / denom
            row_mids = np.clip(color_midpoint + gradient_tilt * norms, 1e-3, 1 - 1e-3)
        shaped = _global_gradient_data(
            xmax,
            resolution=gradient_resolution,
            color_slope=color_slope,
            color_midpoint=color_midpoint,
            scale_mode="anchored" if (green_at is not None and red_at is not None and red_at > green_at) else "linear",
            green_at=green_at,
            red_at=red_at,
            n_rows=use_rows,
            row_midpoints=row_mids,
        )
        y_min = -0.5
        y_max = len(names) - 0.5
        im = ax.imshow(shaped, aspect="auto", extent=[0.0, xmax, y_min, y_max], origin="lower", interpolation="nearest", cmap=cmap, zorder=2.0)
        clip_path = _union_clip_path_from_bars(bars)
        im.set_clip_path(clip_path, transform=ax.transData)
    else:
        t = _positions_from_times(values, scale_mode=scale_mode, time_scale=time_scale, green_at=green_at, red_at=red_at)
        t = [_apply_sigmoid_bias(x, slope=color_slope, midpoint=color_midpoint) for x in t]
        for bar, u in zip(bars, t):
            bar.set_facecolor((1 - u) * g_rgb[0] + u * r_rgb[0], (1 - u) * g_rgb[1] + u * r_rgb[1], (1 - u) * g_rgb[2] + u * r_rgb[2], 1.0)
            bar.set_edgecolor((0, 0, 0, 0))
            bar.set_linewidth(0.0)
    min_val = min(values)
    tol = max(1e-12, 1e-9 * max_val)
    for bar, val, name in zip(bars, values, names):
        is_fastest = abs(val - min_val) <= tol
        ax.text(
            bar.get_width() + (0.012 * xmax),
            bar.get_y() + bar.get_height() / 2,
            format_func(val),
            va="center",
            ha="left",
            color=colors_theme["text"],
            fontsize=11 if is_fastest else 12,
            weight="bold" if is_fastest else "normal",
            zorder=3.0,
        )
    ax.invert_yaxis()
    plt.tight_layout()
    if save_png:
        plt.savefig(str(save_png), dpi=150, bbox_inches="tight", transparent=True)
    if save_svg:
        plt.savefig(str(save_svg), bbox_inches="tight", transparent=True)
    if show:
        bg = _get_theme_background_rgba(theme)
        fig_fc_orig = fig.get_facecolor()
        ax_fc_orig = ax.get_facecolor()
        try:
            fig.patch.set_facecolor(bg)
            ax.set_facecolor(bg)
            fig.canvas.draw_idle()
            plt.show()
        finally:
            fig.patch.set_facecolor(fig_fc_orig)
            ax.set_facecolor(ax_fc_orig)
    return fig, ax


def main():
    artifacts_dir = Path("artifacts")
    jsons = list(artifacts_dir.rglob("*.json"))
    data = {}
    for j in jsons:
        name = j.name
        if "copium" in name:
            variant = "copium"
            parts = name.split("copium-")[1].rsplit(".", 1)[0].split("-")
        else:
            variant = "copy"
            parts = name.split("copy-")[1].rsplit(".", 1)[0].split("-")
        py_tag, platform, arch = parts
        with open(j) as f:
            d = json.load(f)
        benches = {b["name"]: get_mean(b) for b in d.get("benchmarks", [])}
        data.setdefault(platform, {}).setdefault(arch, {}).setdefault(py_tag, {})[variant] = benches

    py_map = {"cp310": "3.10", "cp311": "3.11", "cp312": "3.12", "cp313": "3.13", "cp314": "3.14"}
    benches_list = ["deepcopy", "deepcopy_memo", "deepcopy_reduce"]

    # Select primary platform/arch for charts
    sel_platform = "linux" if "linux" in data else list(data.keys())[0]
    sel_arch = "x86_64" if "x86_64" in data.get(sel_platform, {}) else list(data[sel_platform].keys())[0]
    sel_data = data[sel_platform][sel_arch]

    # Concise deepcopy_memo
    concise_data = [
        (f"copium.deepcopy() - {py_map['cp314']}", sel_data.get("cp314", {}).get("copium", {}).get("deepcopy_memo", 0)),
        (f"copy.deepcopy() ┌ {py_map['cp314']}", sel_data.get("cp314", {}).get("copy", {}).get("deepcopy_memo", 0)),
        (f"┼ {py_map['cp313']}", sel_data.get("cp313", {}).get("copy", {}).get("deepcopy_memo", 0)),
        (f"├ {py_map['cp312']}", sel_data.get("cp312", {}).get("copy", {}).get("deepcopy_memo", 0)),
        (f"├ {py_map['cp311']}", sel_data.get("cp311", {}).get("copy", {}).get("deepcopy_memo", 0)),
        (f"└ {py_map['cp310']}", sel_data.get("cp310", {}).get("copy", {}).get("deepcopy_memo", 0)),
    ]

    # Full memo
    full_memo_copium = [(f"copium.deepcopy() - {py_map[tag]}", sel_data.get(tag, {}).get("copium", {}).get("deepcopy_memo", 0)) for tag in sorted(py_map)]
    full_memo_copy = [(f"copy.deepcopy() - {py_map[tag]}", sel_data.get(tag, {}).get("copy", {}).get("deepcopy_memo", 0)) for tag in sorted(py_map)]

    # Full for each bench
    full_benches = {}
    for bench in benches_list:
        full_benches[bench] = {
            "copium": [(f"copium.deepcopy() - {py_map[tag]}", sel_data.get(tag, {}).get("copium", {}).get(bench, 0)) for tag in sorted(py_map)],
            "copy": [(f"copy.deepcopy() - {py_map[tag]}", sel_data.get(tag, {}).get("copy", {}).get(bench, 0)) for tag in sorted(py_map)],
        }

    # Geomean speedups per python
    geomean_data = []
    for tag in sorted(py_map):
        speedups = []
        for bench in benches_list:
            c = sel_data.get(tag, {}).get("copy", {}).get(bench, 0)
            co = sel_data.get(tag, {}).get("copium", {}).get(bench, 0)
            if co > 0:
                speedups.append(c / co)
        if speedups:
            geomean = np.exp(np.mean(np.log(speedups)))
            geomean_data.append((py_map[tag], geomean))

    # Chart params from example
    FAST_DARK = "#E89B14"
    SLOW_DARK = "#3573A7"
    FAST_LIGHT = "#3573A7"
    SLOW_LIGHT = "#00b3e5"
    mid = 0.17
    slope = 1.6
    satv = 1.1
    valv = 0.95
    tilt = 2

    # Generate charts
    Path("assets").mkdir(exist_ok=True)
    for theme in ["dark", "light"]:
        fast = FAST_DARK if theme == "dark" else FAST_LIGHT
        slow = SLOW_DARK if theme == "light" else SLOW_LIGHT  # Note: swapped in example?

        # Concise
        g_at, r_at = max(v for _, v in concise_data) * 0.95, max(v for _, v in concise_data) * 1.05  # loose
        render_perf_chart(concise_data, theme=theme, order="asc", green=fast, red=slow, color_slope=slope, color_midpoint=mid, gradient_tilt=tilt, sat_scale=satv, val_scale=valv, global_gradient=True, green_at=g_at, red_at=r_at, save_svg=f"assets/chart_{theme}.svg", show=False)

        # Full memo copium + copy
        render_perf_chart(full_memo_copium, theme=theme, order="asc", green=fast, red=slow, color_slope=slope, color_midpoint=mid, gradient_tilt=tilt, sat_scale=satv, val_scale=valv, global_gradient=True, save_svg=f"assets/full_memo_copium_{theme}.svg", show=False)
        render_perf_chart(full_memo_copy, theme=theme, order="asc", green=fast, red=slow, color_slope=slope, color_midpoint=mid, gradient_tilt=tilt, sat_scale=satv, val_scale=valv, global_gradient=True, save_svg=f"assets/full_memo_copy_{theme}.svg", show=False)

        # Full benches
        for bench in benches_list:
            render_perf_chart(full_benches[bench]["copium"], theme=theme, order="asc", green=fast, red=slow, color_slope=slope, color_midpoint=mid, gradient_tilt=tilt, sat_scale=satv, val_scale=valv, global_gradient=True, save_svg=f"assets/full_{bench}_copium_{theme}.svg", show=False)
            render_perf_chart(full_benches[bench]["copy"], theme=theme, order="asc", green=fast, red=slow, color_slope=slope, color_midpoint=mid, gradient_tilt=tilt, sat_scale=satv, val_scale=valv, global_gradient=True, save_svg=f"assets/full_{bench}_copy_{theme}.svg", show=False)

    # Geomean speedup chart (reverse colors for larger=greener)
    for theme in ["dark", "light"]:
        fast = FAST_DARK if theme == "dark" else FAST_LIGHT
        slow = SLOW_DARK if theme == "light" else SLOW_LIGHT
        render_perf_chart(geomean_data, theme=theme, order="desc", green=slow, red=fast, color_slope=slope, color_midpoint=mid, gradient_tilt=tilt, sat_scale=satv, val_scale=valv, global_gradient=True, format_func=_format_speedup, save_svg=f"assets/geomean_speedup_{theme}.svg", show=False)

    # Generate markdown
    md = "# Benchmarks\n\n## Aggregated Results\n\n| Platform | Arch | Python | Variant | deepcopy | deepcopy_memo | deepcopy_reduce |\n|----------|------|--------|---------|----------|---------------|-----------------|\n"
    rows = []
    for platform in data:
        for arch in data[platform]:
            for py_tag in data[platform][arch]:
                for variant in data[platform][arch][py_tag]:
                    b = data[platform][arch][py_tag][variant]
                    rows.append([platform, arch, py_map.get(py_tag, py_tag), variant, b.get("deepcopy", 0), b.get("deepcopy_memo", 0), b.get("deepcopy_reduce", 0)])
    for row in sorted(rows, key=lambda r: (r[0], r[1], r[2], r[3])):
        md_row = row[:4] + [_format_time_s(x) for x in row[4:]]
        md += "| " + " | ".join(map(str, md_row)) + " |\n"

    md += "\n## Speedups Geomean\n\n| Python | Geomean Speedup |\n|--------|-----------------|\n"
    for label, geo in sorted(geomean_data):
        md += f"| {label} | {_format_speedup(geo)} |\n"

    md += "\n## Charts\n\n### Concise deepcopy_memo\n![dark](assets/chart_dark.svg)\n![light](assets/chart_light.svg)\n"
    md += "### Full deepcopy_memo\n![copium dark](assets/full_memo_copium_dark.svg)\n![copium light](assets/full_memo_copium_light.svg)\n![copy dark](assets/full_memo_copy_dark.svg)\n![copy light](assets/full_memo_copy_light.svg)\n"
    for bench in benches_list:
        md += f"### Full {bench}\n![copium dark](assets/full_{bench}_copium_dark.svg)\n![copium light](assets/full_{bench}_copium_light.svg)\n![copy dark](assets/full_{bench}_copy_dark.svg)\n![copy light](assets/full_{bench}_copy_light.svg)\n"
    md += "### Geomean Speedups\n![dark](assets/geomean_speedup_dark.svg)\n![light](assets/geomean_speedup_light.svg)\n"

    with open("BENCHMARKS.md", "w") as f:
        f.write(md)


if __name__ == "__main__":
    main()