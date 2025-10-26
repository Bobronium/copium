# SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium) <mail@bobronium.me>
from typing import Final
from typing import NewType
from typing import TypeVar

T = TypeVar("T")

XT = NewType("XT", tuple[int, str, dict[str, str]])  # ty doesn't understand TypeAlias yet.
X: Final[XT] = XT((1, "2", {"3": "4"}))

__all__ = ["XT", "X"]
