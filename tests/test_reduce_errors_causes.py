"""
Test that on top of errors equivalent to stdlib, copium also chains descriptive __cause__.
"""

import copyreg

import pytest

import copium


class ArgtupNotIterable:
    def __reduce__(self):
        return (type(self), 42)


class NewObjExArgsNotIterable:
    def __reduce__(self):
        return (copyreg.__newobj_ex__, (type(self), 42, {}))


class NewObjExKwargsNotMapping:
    def __reduce__(self):
        return (copyreg.__newobj_ex__, (type(self), (), 42))


class DictStateNotMapping:
    def __reduce__(self):
        return (type(self), (), [1, 2, 3])


class SlotStateNoItems:
    def __reduce__(self):
        return (type(self), (), ({}, 42))


@pytest.mark.parametrize(
    "obj, cause_message",
    [
        pytest.param(
            ArgtupNotIterable(),
            "second element of the tuple returned by ArgtupNotIterable.__reduce__"
            " must be a tuple, not int",
            id="argtup-not-iterable",
        ),
        pytest.param(
            NewObjExArgsNotIterable(),
            "__newobj_ex__ args in NewObjExArgsNotIterable.__reduce__ result"
            " must be a tuple, not int",
            id="newobj_ex-args-not-iterable",
        ),
        pytest.param(
            NewObjExKwargsNotMapping(),
            "__newobj_ex__ kwargs in NewObjExKwargsNotMapping.__reduce__ result"
            " must be a dict, not int",
            id="newobj_ex-kwargs-not-mapping",
        ),
        pytest.param(
            DictStateNotMapping(),
            "dict state from DictStateNotMapping.__reduce__"
            " must be a dict or mapping, got list",
            id="dict-state-not-mapping",
        ),
        pytest.param(
            SlotStateNoItems(),
            "slot state from SlotStateNoItems.__reduce__"
            " must be a dict or have an items() method, got int",
            id="slot-state-no-items",
        ),
    ],
)
def test_reduce_chained_type_error(obj, cause_message):
    with pytest.raises(Exception) as exc_info:
        copium.deepcopy(obj)

    cause = exc_info.value.__cause__
    assert cause is not None, f"expected chained __cause__, got bare {exc_info.value!r}"
    assert isinstance(cause, TypeError), f"expected TypeError cause, got {type(cause).__name__}: {cause}"
    assert str(cause) == str(cause_message)
