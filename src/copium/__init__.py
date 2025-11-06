"""Copium - Ultra-fast deepcopy for Python."""
from .copium import *  # noqa: F401, F403

__doc__ = copium.__doc__  # noqa: F405
if hasattr(copium, "__all__"):  # noqa: F405
    __all__ = copium.__all__  # noqa: F405
