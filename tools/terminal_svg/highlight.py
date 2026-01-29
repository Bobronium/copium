from __future__ import annotations

import re
from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from tools.terminal_svg.themes import Theme

KEYWORDS = frozenset({
    "False", "None", "True", "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield",
})

BUILTINS = frozenset({
    "abs", "all", "any", "bin", "bool", "bytes", "callable", "chr",
    "classmethod", "compile", "complex", "delattr", "dict", "dir",
    "divmod", "enumerate", "eval", "exec", "filter", "float", "format",
    "frozenset", "getattr", "globals", "hasattr", "hash", "help", "hex",
    "id", "input", "int", "isinstance", "issubclass", "iter", "len",
    "list", "locals", "map", "max", "memoryview", "min", "next", "object",
    "oct", "open", "ord", "pow", "print", "property", "range", "repr",
    "reversed", "round", "set", "setattr", "slice", "sorted", "staticmethod",
    "str", "sum", "super", "tuple", "type", "vars", "zip",
})

TOKEN_PATTERNS = [
    ("COMMENT", r"#[^\n]*"),
    ("STRING", r'"""[\s\S]*?"""|\'\'\'[\s\S]*?\'\'\'|"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\''),
    ("NUMBER", r"\b(?:0[xXoObB][\da-fA-F_]+|\d[\d_]*(?:\.[\d_]*)?(?:[eE][+-]?\d+)?j?)\b"),
    ("MAGIC", r"%\w+"),
    ("OPERATOR", r"[+\-*/%@&|^~<>=!:;,.\[\]{}()]"),
    ("NAME", r"\b[a-zA-Z_]\w*\b"),
    ("SPACE", r"[ \t]+"),
    ("NEWLINE", r"\n"),
    ("OTHER", r"."),
]

TOKEN_RE = re.compile("|".join(f"(?P<{name}>{pattern})" for name, pattern in TOKEN_PATTERNS))


@dataclass(frozen=True)
class Token:
    kind: str
    text: str


def tokenize(code: str) -> list[Token]:
    tokens = []
    for match in TOKEN_RE.finditer(code):
        kind = match.lastgroup
        text = match.group()
        if kind == "NAME":
            if text in KEYWORDS:
                kind = "KEYWORD"
            elif text in BUILTINS:
                kind = "BUILTIN"
        tokens.append(Token(kind, text))
    return tokens


def get_token_color(token: Token, theme: Theme) -> str:
    mapping = {
        "KEYWORD": theme.keyword,
        "BUILTIN": theme.builtin,
        "NUMBER": theme.number,
        "STRING": theme.string,
        "MAGIC": theme.keyword,
        "OPERATOR": theme.text,
        "COMMENT": theme.comment,
    }
    return mapping.get(token.kind, theme.text)
