from __future__ import annotations

from typing import TYPE_CHECKING

import svg

from tools.terminal_svg.highlight import Token, get_token_color, tokenize

from tools.terminal_svg.elements import (
    Bar,
    CellContent,
    In,
    IPython,
    Out,
    TerminalWindow,
    TimeitBar,
)
from tools.terminal_svg.themes import Theme

FONT_MONO = "SF Mono, Menlo, Consolas, monospace"
FONT_SIZE = 13
LINE_HEIGHT = 26
TITLE_BAR_HEIGHT = 32
PADDING_X = 20
PADDING_Y = 16
BAR_HEIGHT = 14
BAR_LABEL_GAP = 10
CHAR_WIDTH = 7.8
MIN_BAR_WIDTH = 150


def calculate_content_width(ipython: IPython) -> int:
    required_widths: list[float] = []

    max_time_label_width = 0.0
    for cell in ipython.cells:
        if isinstance(cell, TimeitBar):
            max_time_label_width = max(max_time_label_width, len(cell.format_time()) * CHAR_WIDTH)

    for cell in ipython.cells:
        if isinstance(cell, In):
            prompt_len = len(f"In [{cell.cell_number or 1}]: ")
            line_width = 2 * PADDING_X + (prompt_len + len(cell.code)) * CHAR_WIDTH
            required_widths.append(line_width)
        elif isinstance(cell, Out):
            line_width = 2 * PADDING_X + len(cell.text) * CHAR_WIDTH
            required_widths.append(line_width)
        elif isinstance(cell, TimeitBar):
            line_width = 2 * PADDING_X + MIN_BAR_WIDTH + BAR_LABEL_GAP + max_time_label_width
            required_widths.append(line_width)
        elif isinstance(cell, Bar):
            label_width = len(cell.label) * CHAR_WIDTH if cell.label else 0
            line_width = 2 * PADDING_X + MIN_BAR_WIDTH + BAR_LABEL_GAP + label_width
            required_widths.append(line_width)

    return int(max(required_widths)) if required_widths else 400


def render_terminal(window: TerminalWindow, theme: Theme) -> str:
    if window.width is None:
        natural_width = calculate_content_width(window.content)
        effective_width = max(window.min_width, min(window.max_width, natural_width))
    else:
        effective_width = max(window.min_width, min(window.max_width, window.width))
    body_elements, body_height = render_ipython(window.content, theme, effective_width)

    if window.chrome:
        total_height = TITLE_BAR_HEIGHT + body_height + PADDING_Y
        total_height = max(total_height, window.min_height)
        content_y_offset = TITLE_BAR_HEIGHT
    else:
        total_height = body_height
        content_y_offset = 0

    elements: list[svg.Element] = [
        svg.Defs(elements=[
            svg.LinearGradient(
                id="barGradSlow",
                x1="0%", y1="0%", x2="100%", y2="0%",
                elements=[
                    svg.Stop(offset="0%", stop_color=theme.slow_gradient_start, stop_opacity=1),
                    svg.Stop(offset="100%", stop_color=theme.slow_gradient_end, stop_opacity=1),
                ],
            ),
        ]),
    ]

    if window.chrome:
        elements.extend([
            svg.Rect(width=effective_width, height=total_height, rx=12, fill=theme.bg),
            svg.Rect(width=effective_width, height=TITLE_BAR_HEIGHT, rx=12, fill=theme.title_bar),
            svg.Rect(y=20, width=effective_width, height=12, fill=theme.title_bar),
            svg.Circle(cx=20, cy=16, r=6, fill="#ff5f56"),
            svg.Circle(cx=40, cy=16, r=6, fill="#ffbd2e"),
            svg.Circle(cx=60, cy=16, r=6, fill="#27ca40"),
            svg.Text(
                x=effective_width // 2,
                y=20,
                fill=theme.dim,
                font_family=FONT_MONO,
                font_size=12,
                text_anchor="middle",
                text=window.title,
            ),
        ])

    elements.append(svg.G(
        style=f"font-family: {FONT_MONO}; font-size: {FONT_SIZE}px; white-space: pre",
        transform=f"translate(0, {content_y_offset})" if content_y_offset else None,
        elements=body_elements,
    ))

    return str(svg.SVG(
        viewBox=svg.ViewBoxSpec(0, 0, effective_width, total_height),
        elements=elements,
    ))


def render_ipython(ipython: IPython, theme: Theme, width: int) -> tuple[list[svg.Element], int]:
    elements: list[svg.Element] = []
    y = PADDING_Y

    max_time = 0.0
    max_time_label_width = 0.0
    for cell in ipython.cells:
        if isinstance(cell, TimeitBar):
            max_time = max(max_time, cell.seconds)
            max_time_label_width = max(max_time_label_width, len(cell.format_time()) * CHAR_WIDTH)

    max_bar_width = width - 2 * PADDING_X - BAR_LABEL_GAP - max_time_label_width

    for cell in ipython.cells:
        if isinstance(cell, In):
            cell_elements, cell_height = render_input(cell, theme, y, width)
            elements.extend(cell_elements)
            y += cell_height
        elif isinstance(cell, Out):
            cell_elements, cell_height = render_output(cell, theme, y)
            elements.extend(cell_elements)
            y += cell_height
        elif isinstance(cell, TimeitBar):
            cell_elements, cell_height = render_timeit_bar(
                cell, theme, y, max_bar_width, max_time, width
            )
            elements.extend(cell_elements)
            y += cell_height
        elif isinstance(cell, Bar):
            bar_width = int(max_bar_width * cell.width_fraction)
            elements.append(svg.Rect(
                x=PADDING_X, y=y, width=bar_width, height=BAR_HEIGHT, rx=2, fill=cell.color
            ))
            if cell.label:
                elements.append(svg.Text(
                    x=PADDING_X + bar_width + 8, y=y + 11, fill=theme.text, text=cell.label
                ))
            y += LINE_HEIGHT

    return elements, y


def render_input(cell: In, theme: Theme, y: int, width: int) -> tuple[list[svg.Element], int]:
    prompt = f"In [{cell.cell_number}]: "
    prompt_width = len(prompt) * CHAR_WIDTH
    content_width = width - PADDING_X - prompt_width - PADDING_X

    lines = split_code_lines(cell.code, content_width)
    elements: list[svg.Element] = []

    for i, line in enumerate(lines):
        line_y = y + (i + 1) * LINE_HEIGHT - 10
        if i == 0:
            prefix = prompt
        else:
            prefix = "\u00a0" * 6 + ": "

        tspans = [svg.TSpan(text=prefix, fill=theme.dim)]
        tspans.extend(highlight_code(line, theme))
        elements.append(svg.Text(x=PADDING_X, y=line_y, elements=tspans))

    return elements, len(lines) * LINE_HEIGHT


def render_output(cell: Out, theme: Theme, y: int) -> tuple[list[svg.Element], int]:
    line_y = y + LINE_HEIGHT - 10
    elements = [svg.Text(x=PADDING_X, y=line_y, fill=theme.text, text=cell.text)]
    return elements, LINE_HEIGHT


def render_timeit_bar(
    cell: TimeitBar,
    theme: Theme,
    y: int,
    max_bar_width: int,
    max_time: float,
    width: int,
) -> tuple[list[svg.Element], int]:
    fraction = cell.seconds / max_time if max_time > 0 else 1.0
    bar_width = max(1, int(max_bar_width * fraction))

    bar_color = theme.fast if cell.style == "fast" else "url(#barGradSlow)"
    opacity = None if cell.style == "fast" else (0.8 if theme.name == "dark" else 0.9)

    bar_y = y + (LINE_HEIGHT - BAR_HEIGHT) // 2
    text_baseline_y = bar_y + BAR_HEIGHT - 3

    elements: list[svg.Element] = [
        svg.Rect(
            x=PADDING_X,
            y=bar_y,
            width=bar_width,
            height=BAR_HEIGHT,
            rx=2,
            fill=bar_color,
            opacity=opacity,
        ),
    ]

    time_color = theme.fast if cell.style == "fast" else theme.slow
    time_label_x = PADDING_X + bar_width + BAR_LABEL_GAP
    elements.append(svg.Text(
        x=time_label_x,
        y=text_baseline_y,
        elements=[svg.TSpan(text=cell.format_time(), fill=time_color, font_weight="600")],
    ))

    speedup = cell.speedup_label()
    if speedup and cell.style == "fast" and cell.baseline is not None:
        baseline_fraction = cell.baseline.seconds / max_time if max_time > 0 else 1.0
        baseline_bar_width = max(1, int(max_bar_width * baseline_fraction))
        baseline_label_x = PADDING_X + baseline_bar_width + BAR_LABEL_GAP
        baseline_label_end_x = baseline_label_x + len(cell.baseline.format_time()) * CHAR_WIDTH

        arrow_text = "◂── "
        elements.append(svg.Text(
            x=baseline_label_end_x,
            y=text_baseline_y,
            text_anchor="end",
            elements=[
                svg.TSpan(text=arrow_text, fill=theme.dim),
                svg.TSpan(text=speedup, fill=theme.dim),
            ],
        ))

    return elements, LINE_HEIGHT


def split_code_lines(code: str, max_width: float) -> list[str]:
    explicit_lines = code.split("\n")
    result = []
    for line in explicit_lines:
        if len(line) * CHAR_WIDTH <= max_width:
            result.append(line)
        else:
            result.extend(wrap_line(line, max_width))
    return result


def wrap_line(line: str, max_width: float) -> list[str]:
    max_chars = int(max_width / CHAR_WIDTH)
    if max_chars < 20:
        return [line]

    result = []
    remaining = line
    while remaining:
        if len(remaining) <= max_chars:
            result.append(remaining)
            break

        break_at = max_chars
        for delim in [", ", " ", "(", "[", "{"]:
            idx = remaining.rfind(delim, 0, max_chars)
            if idx > max_chars // 2:
                break_at = idx + len(delim)
                break

        result.append(remaining[:break_at])
        remaining = remaining[break_at:].lstrip()

    return result


def highlight_code(code: str, theme: Theme) -> list[svg.TSpan]:
    tokens = tokenize(code)
    tspans = []
    for token in tokens:
        if token.kind in ("SPACE", "NEWLINE"):
            tspans.append(svg.TSpan(text=token.text))
        else:
            color = get_token_color(token, theme)
            tspans.append(svg.TSpan(text=token.text, fill=color))
    return tspans
