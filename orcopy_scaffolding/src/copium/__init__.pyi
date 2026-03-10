import sys
from copy import Error
from typing import Any, Literal, Sequence, TypeVar, TypedDict

from copium import patch, config

__all__ = ["copy", "deepcopy", "Error", "patch", "config"]

T = TypeVar("T")

def copy(x: T) -> T:
    """Return a shallow copy of x."""

def deepcopy(x: T, memo: dict[int, Any] | None = None) -> T:
    """Return a deep copy of x."""

if sys.version_info >= (3, 13):
    def replace(obj: T, /, **changes: Any) -> T:
        """Create a new object of the same type as obj, replacing fields with values from changes."""
    __all__.append("replace")


class _CopiumConfig(TypedDict, total=True):
    memo: Literal["native", "dict"]
    on_incompatible: Literal["warn", "raise", "silent"]
    suppress_warnings: tuple[str, ...]


class config:
    """Configuration submodule: copium.config.apply(...) / copium.config.get()"""

    @staticmethod
    def apply(
        *,
        memo: Literal["native", "dict"] | None = None,
        on_incompatible: Literal["warn", "raise", "silent"] | None = None,
        suppress_warnings: Sequence[str] | None = None,
    ) -> None:
        """
        Update copium configuration. Only specified arguments are changed.
        Called with no arguments, resets all settings to environment variable defaults.

        :param memo: 'native' (fast, default) or 'dict' (universally compatible).
        :param on_incompatible: What to do when __deepcopy__ rejects native memo.
            'warn' emits a warning and retries with dict (default).
            'raise' lets the error propagate.
            'silent' retries with dict silently.
            Only relevant when memo='native'.
        :param suppress_warnings: Sequence of error strings to suppress warnings for.
            Pass an empty sequence [] to clear. Omit to leave unchanged.
        """

    @staticmethod
    def get() -> _CopiumConfig:
        """Return the current configuration."""


class patch:
    """Monkey-patching utilities for stdlib copy module."""

    @staticmethod
    def enable() -> bool:
        """
        Patch copy.deepcopy to use copium. Idempotent.

        :return: True if state changed, False otherwise.
        """

    @staticmethod
    def disable() -> bool:
        """
        Restore original copy.deepcopy. Idempotent.

        :return: True if state changed, False otherwise.
        """

    @staticmethod
    def enabled() -> bool:
        """:return: Whether copy.deepcopy is currently patched."""
