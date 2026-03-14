from typing import Literal, Sequence, TypedDict, overload

__all__ = ["apply", "get"]

@overload
def apply(
    *,
    memo: Literal["native"],
    on_incompatible: Literal["warn"] = "warn",
    suppress_warnings: None = None,
) -> None:
    """Default configuration."""

@overload
def apply(*, memo: Literal["dict"]) -> None:
    """Use stdlib-compatible dict memo. 100% parity with stdlib."""

@overload
def apply(*, memo: Literal["native"]) -> None:
    """
    Use fast and lightweight `copium.memo`. Incompatible with some `__deepcopy__` implementations.
    """

@overload
def apply(
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

def get() -> _CopiumConfig:
    """
    Return the current configuration as a dict.
    """
