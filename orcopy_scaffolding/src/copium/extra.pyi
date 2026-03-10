from typing import Callable
from typing import TypeVar

__all__ = ["repeatcall", "replicate"]

T = TypeVar("T")

def repeatcall(function: Callable[[], T], size: int, /) -> list[T]:
    """
    Call function repeatedly size times and return the list of results.

    Equivalent of [function() for _ in range(size)], but faster.
    """

def replicate(obj: T, /, n: int) -> list[T]:
    """
    Returns n copies of the object in a list.

    Equivalent of [deepcopy(obj) for _ in range(n)], but faster.
    """
