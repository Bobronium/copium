import sys
from copy import Error
from typing import Any, Literal, Sequence, TypeVar, overload, TypedDict

from copium import patch

__all__ = ["copy", "deepcopy", "Error", "configure", "get_config", "patch"]

T = TypeVar("T")

def copy(x: T) -> T:
    """
    Natively compiled copy.

    :param x: object to copy.
    :return: shallow copy of the `x`.
    """

def deepcopy(x: T, memo: dict[int, Any] | None = None) -> T:
    """
    Natively compiled deepcopy.

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

@overload
def configure() -> None:
    """Reset all settings to environment variable defaults."""

@overload
def configure(
    *,
    memo: Literal["native"],
    on_incompatible: Literal["warn"] = "warn",
    suppress_warnings: None = None,
) -> None:
    """Default configuration."""

@overload
def configure(*, memo: Literal["dict"]) -> None:
    """Use stdlib-compatible dict memo. 100% parity with stdlib."""

@overload
def configure(*, memo: Literal["native"]) -> None:
    """
    Use fast and lightweight `copium.memo`. Incompatible with some `__deepcopy__` implementations.
    """

@overload
def configure(
    *,
    memo: Literal["native"] = ...,
    on_incompatible: Literal["warn", "raise", "silent"] = ...,
    suppress_warnings: Sequence[str] | None = ...,
) -> None:
    """
    Configure copium behavior. Only specified arguments are changed.

    :param memo: 'native' (fast, default) or 'dict' (universally compatible).
    :param on_incompatible: What to do when __deepcopy__ rejects native memo.
        'warn' emits a warning and retries with dict (default).
        'raise' lets the error propagate.
        'silent' retries with dict silently.
        Only relevant when memo='native'.
    :param suppress_warnings: Error strings to suppress warnings for.
        None clears the list.
    """

class _CopiumConfig(TypedDict, total=True):
    memo: Literal["native", "dict"]
    on_incompatible: Literal["warn", "raise", "silent"]
    suppress_warnings: tuple[str, ...]

def get_config() -> _CopiumConfig:
    """
    Return the current configuration as a dict.
    """
