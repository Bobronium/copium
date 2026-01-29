from __future__ import annotations

from dataclasses import dataclass
from typing import Literal


@dataclass(frozen=True)
class Theme:
    name: Literal["dark", "light"]
    bg: str
    title_bar: str
    dim: str
    text: str
    keyword: str
    builtin: str
    number: str
    string: str
    operator: str
    comment: str
    fast: str
    slow: str
    slow_gradient_start: str
    slow_gradient_end: str


DARK = Theme(
    name="dark",
    bg="#0d1117",
    title_bar="#161b22",
    dim="#8b949e",
    text="#c9d1d9",
    keyword="#ff7b72",
    builtin="#79c0ff",
    number="#79c0ff",
    string="#a5d6ff",
    operator="#ff7b72",
    comment="#8b949e",
    fast="#3fb950",
    slow="#f85149",
    slow_gradient_start="#BFA14A",  # warm, slightly yellowed gold
    slow_gradient_end="#8B4513",    # keeps the grounded brown anchor
    # slow_gradient_start="#A65E44",
    # slow_gradient_end="#8B4513",
    # slow_gradient_start="#cd7f32",
    # slow_gradient_end="#b8860b",
)

LIGHT = Theme(
    name="light",
    bg="#f6f8fa",
    title_bar="#e1e4e8",
    dim="#6a737d",
    text="#24292e",
    keyword="#d73a49",
    builtin="#005cc5",
    number="#005cc5",
    string="#032f62",
    operator="#d73a49",
    comment="#6a737d",
    fast="#22863a",
    slow="#cb2431",
    slow_gradient_start="#E4B87A",
    slow_gradient_end="#A0522D",
)
