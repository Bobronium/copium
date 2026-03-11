__all__ = ["enable", "disable", "enabled"]

def enable() -> bool:
    """
    Patch copy.deepcopy to use copium. Idempotent.

    :return: True if state changed, False otherwise.
    """

def disable() -> bool:
    """
    Restore original copy.deepcopy. Idempotent.

    :return: True if state changed, False otherwise.
    """

def enabled() -> bool:
    """
    :return: Whether copy.deepcopy is patched.
    """
