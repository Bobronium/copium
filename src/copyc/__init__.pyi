import sys
from copy import Error
from typing import Any, TypeVar

__all__ = ["copy", "deepcopy", "Error"]

T = TypeVar("T")

def copy(x: T) -> T:
    """
    Return a shallow copy of obj.

    :param x: object to copy.
    :return: shallow copy of the `x`.
    """

def deepcopy(x: T, memo: dict[int, Any] | None = None) -> T:
    """
    Return a deep copy of obj.

    :param x: object to deepcopy
    :param memo: treat as opaque.
    :return: deep copy of the `x`.
    """

if sys.version_info >= (3, 13):
    def replace(obj: T, /, **changes: Any) -> T:
        """
        Creates a new object of the same type as obj, replacing fields with values from changes.
        """
    __all__.append("replace")
