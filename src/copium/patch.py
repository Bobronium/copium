"""Simple Python-based patching for copium.deepcopy."""
import copy as _copy_module
import copium as _copium_module

_original_deepcopy = None
_is_enabled = False


def enable():
    """Replace copy.deepcopy with copium.deepcopy."""
    global _original_deepcopy, _is_enabled
    if _is_enabled:
        return

    _original_deepcopy = _copy_module.deepcopy
    _copy_module.deepcopy = _copium_module.deepcopy
    _is_enabled = True


def disable():
    """Restore original copy.deepcopy."""
    global _original_deepcopy, _is_enabled
    if not _is_enabled or _original_deepcopy is None:
        return

    _copy_module.deepcopy = _original_deepcopy
    _is_enabled = False


def enabled():
    """Check if patching is enabled."""
    return _is_enabled
