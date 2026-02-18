from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Literal

if TYPE_CHECKING:
    from tools.terminal_svg.themes import Theme


@dataclass
class In:
    """IPython input cell."""
    code: str
    cell_number: int | None = None


@dataclass
class Out:
    """IPython output (text)."""
    text: str
    cell_number: int | None = None


@dataclass
class Bar:
    """Generic progress/result bar."""
    width_fraction: float
    color: str
    label: str = ""


@dataclass
class TimeitBar:
    """Bar representing %timeit result with automatic formatting."""
    seconds: float
    style: Literal["fast", "slow"]
    baseline: TimeitBar | None = None

    def format_time(self) -> str:
        s = self.seconds
        if s >= 1:
            return f"{s:.2f} s" if s < 10 else f"{s:.1f} s"
        ms = s * 1000
        if ms >= 1:
            return f"{ms:.1f} ms" if ms >= 10 else f"{ms:.2f} ms"
        us = s * 1_000_000
        return f"{us:.1f} µs" if us >= 10 else f"{us:.2f} µs"

    def speedup_label(self) -> str | None:
        if self.baseline is None:
            return None
        ratio = self.baseline.seconds / self.seconds
        return f"{ratio:.1f}× faster"


CellContent = In | Out | TimeitBar | Bar


@dataclass
class IPython:
    """IPython session containing cells."""
    cells: list[CellContent] = field(default_factory=list)

    def __init__(self, *cells: CellContent):
        self.cells = list(cells)
        self._assign_cell_numbers()

    def _assign_cell_numbers(self) -> None:
        n = 1
        for cell in self.cells:
            if isinstance(cell, In):
                if cell.cell_number is None:
                    cell.cell_number = n
                n = cell.cell_number + 1
            elif isinstance(cell, Out):
                if cell.cell_number is None:
                    cell.cell_number = n - 1


@dataclass
class TerminalWindow:
    """macOS-style terminal window containing an IPython session."""
    content: IPython
    title: str = "ipython"
    theme: Theme | None = None
    width: int | None = None
    min_width: int = 400
    max_width: int = 1200
    min_height: int = 100
    chrome: bool = True

    def render(self, theme: Theme | None = None) -> str:
        from tools.terminal_svg.render import render_terminal

        t = theme or self.theme
        if t is None:
            from tools.terminal_svg.themes import DARK
            t = DARK
        return render_terminal(self, t)

    def save(self, path: str, theme: Theme | None = None) -> None:
        from pathlib import Path
        Path(path).write_text(self.render(theme))
