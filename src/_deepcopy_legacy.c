/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_DEEPCOPY_LEGACY_C
#define _COPIUM_DEEPCOPY_LEGACY_C

#include "_common.h"
#include "_state.c"
#include "_dict_iter.c"
#include "_reduce_helpers.c"
#include "_type_checks.c"
#include "_recursion_guard.c"
#include "_memo_legacy.c"

/* _PySet_NextEntry() */
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
    #include "pycore_setobject.h"
#endif

static ALWAYS_INLINE PyObject* deepcopy_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);

static MAYBE_INLINE PyObject* deepcopy_list_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static MAYBE_INLINE PyObject* deepcopy_tuple_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static MAYBE_INLINE PyObject* deepcopy_dict_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static MAYBE_INLINE PyObject* deepcopy_set_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static MAYBE_INLINE PyObject* deepcopy_frozenset_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static MAYBE_INLINE PyObject* deepcopy_bytearray_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static MAYBE_INLINE PyObject* deepcopy_method_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
);
static PyObject* deepcopy_object_legacy(
    PyObject* original, PyTypeObject* tp, PyObject* memo, PyObject** keepalive_pointer
);

static ALWAYS_INLINE PyObject* deepcopy_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    PyTypeObject* type = Py_TYPE(original);

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    if (LIKELY(is_literal_immutable(type) || is_builtin_immutable(type) || is_class(type)))
        return Py_NewRef(original);
#endif

    void* memo_key = (void*)original;
    PyObject* memoized = memo_lookup_legacy(memo, memo_key);
    if (memoized)
        return memoized;
    if (PyErr_Occurred())
        return NULL;

#if PY_VERSION_HEX < PY_VERSION_3_14_HEX
    if (LIKELY(is_literal_immutable(type)))
        return Py_NewRef(original);
#endif

    if (type == &PyList_Type)
        return RECURSION_GUARDED(deepcopy_list_legacy(original, memo, keepalive_pointer));
    if (type == &PyTuple_Type)
        return RECURSION_GUARDED(deepcopy_tuple_legacy(original, memo, keepalive_pointer));
    if (type == &PyDict_Type)
        return RECURSION_GUARDED(deepcopy_dict_legacy(original, memo, keepalive_pointer));
    if (type == &PySet_Type)
        return RECURSION_GUARDED(deepcopy_set_legacy(original, memo, keepalive_pointer));

    if (is_builtin_immutable(type) || is_class(type))
        return Py_NewRef(original);

    if (type == &PyFrozenSet_Type)
        return RECURSION_GUARDED(deepcopy_frozenset_legacy(original, memo, keepalive_pointer));
    if (type == &PyByteArray_Type)
        return deepcopy_bytearray_legacy(original, memo, keepalive_pointer);
    if (type == &PyMethod_Type)
        return deepcopy_method_legacy(original, memo, keepalive_pointer);

    if (is_stdlib_immutable(type))
        return Py_NewRef(original);

    PyObject* __deepcopy__ = NULL;
    int has_deepcopy = PyObject_GetOptionalAttr(original, module_state.s__deepcopy__, &__deepcopy__);
    if (has_deepcopy < 0)
        return NULL;
    if (has_deepcopy) {
        PyObject* copied = PyObject_CallOneArg(__deepcopy__, memo);
        Py_DECREF(__deepcopy__);

        if (!copied)
            return NULL;

        if (copied != original) {
            if (memoize_legacy(memo, memo_key, copied) < 0) {
                Py_DECREF(copied);
                return NULL;
            }
            if (keepalive_legacy(memo, keepalive_pointer, original) < 0) {
                Py_DECREF(copied);
                return NULL;
            }
        }
        return copied;
    }

    return deepcopy_object_legacy(original, type, memo, keepalive_pointer);
}

static MAYBE_INLINE PyObject* deepcopy_list_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    Py_ssize_t sz = PyList_GET_SIZE(original);

    PyObject* copied = PyList_New(sz);
    if (!copied)
        return NULL;

    for (Py_ssize_t i = 0; i < sz; i++) {
#if PY_VERSION_HEX < PY_VERSION_3_12_HEX
        Py_INCREF(Py_Ellipsis);
#endif
        PyList_SET_ITEM(copied, i, Py_Ellipsis);
    }

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < sz; i++) {
        PyObject* item = COPIUM_PyList_GET_ITEM_REF(original, i);
        if (UNLIKELY(item == NULL)) {
            PyErr_SetString(PyExc_RuntimeError, "list changed size during iteration");
            goto error;
        }

        Py_SETREF(item, deepcopy_legacy(item, memo, keepalive_pointer));

        if (!item)
            goto error;

        COPIUM_Py_BEGIN_CRITICAL_SECTION(copied);
        if (UNLIKELY(PyList_GET_SIZE(copied) != sz)) {
            Py_DECREF(item);
            PyErr_SetString(PyExc_RuntimeError, "list changed size during iteration");
            goto error;
        }
#if PY_VERSION_HEX < PY_VERSION_3_12_HEX
        PyList_SetItem(copied, i, item);
#else
        PyList_SET_ITEM(copied, i, item);
#endif
        COPIUM_Py_END_CRITICAL_SECTION();
    }

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0)
        goto error;

    return copied;

error:
    Py_DECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_tuple_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    Py_ssize_t sz = PyTuple_GET_SIZE(original);

    PyObject* copied = PyTuple_New(sz);
    if (!copied)
        return NULL;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; i++) {
        PyObject* item = PyTuple_GET_ITEM(original, i);
        PyObject* copied_item = deepcopy_legacy(item, memo, keepalive_pointer);
        if (!copied_item) {
            Py_DECREF(copied);
            return NULL;
        }
        if (copied_item != item)
            all_same = 0;
        PyTuple_SET_ITEM(copied, i, copied_item);
    }

    if (all_same) {
        Py_DECREF(copied);
        return Py_NewRef(original);
    }

    PyObject* existing = memo_lookup_legacy(memo, (void*)original);
    if (existing) {
        Py_DECREF(copied);
        return existing;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(copied);
        return NULL;
    }

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    return copied;
}

static MAYBE_INLINE PyObject* deepcopy_dict_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    PyObject* copied = _PyDict_NewPresized(PyDict_Size(original));
    if (!copied)
        return NULL;

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    DictIterGuard iter_guard;
    if (dict_iter_init(&iter_guard, original) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    PyObject *key, *value;
    int iter_flag;

    while ((iter_flag = dict_iter_next(&iter_guard, &key, &value)) > 0) {
        Py_SETREF(key, deepcopy_legacy(key, memo, keepalive_pointer));
        if (!key) {
            Py_DECREF(value);
            goto error;
        }

        Py_SETREF(value, deepcopy_legacy(value, memo, keepalive_pointer));
        if (!value) {
            Py_DECREF(key);
            goto error;
        }

        if (COPIUM_PyDict_SetItem_Take2((PyDictObject*)copied, key, value) < 0)
            goto error;
    }

    if (iter_flag < 0)
        goto error;

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0)
        goto error;

    return copied;

error:
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    dict_iter_cleanup(&iter_guard);
#endif
    Py_DECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_set_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    PyObject* snapshot;
    PyObject* item;
    Py_ssize_t i;

    COPIUM_Py_BEGIN_CRITICAL_SECTION(original) Py_ssize_t sz = PySet_Size(original);
    if (sz < 0)
        return NULL;

    snapshot = PyTuple_New(sz);
    if (!snapshot)
        return NULL;

    Py_ssize_t pos = 0;
    i = 0;
    Py_hash_t hash;
    while (_PySet_NextEntry(original, &pos, &item, &hash)) {
        PyTuple_SET_ITEM(snapshot, i++, Py_NewRef(item));
    }

    COPIUM_Py_END_CRITICAL_SECTION();

    PyObject* copied = PySet_New(NULL);
    if (!copied) {
        Py_DECREF(snapshot);
        return NULL;
    }

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(snapshot);
        Py_DECREF(copied);
        return NULL;
    }

    for (Py_ssize_t j = 0; j < i; j++) {
        item = PyTuple_GET_ITEM(snapshot, j);
        PyObject* copied_item = deepcopy_legacy(item, memo, keepalive_pointer);
        if (!copied_item)
            goto error;

        int ret = PySet_Add(copied, copied_item);
        Py_DECREF(copied_item);

        if (ret < 0)
            goto error;
    }

    Py_DECREF(snapshot);

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    return copied;

error:
    Py_DECREF(snapshot);
    Py_DECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    Py_ssize_t sz = PySet_Size(original);
    if (sz < 0)
        return NULL;

    PyObject* items = PyTuple_New(sz);
    if (!items)
        return NULL;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(original, &pos, &item, &hash) && i < sz) {
        PyObject* copied_item = deepcopy_legacy(item, memo, keepalive_pointer);
        if (!copied_item) {
            Py_DECREF(items);
            return NULL;
        }
        PyTuple_SET_ITEM(items, i++, copied_item);
    }

    PyObject* copied = PyFrozenSet_New(items);
    Py_DECREF(items);

    if (!copied)
        return NULL;

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    return copied;
}

static MAYBE_INLINE PyObject* deepcopy_bytearray_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    Py_ssize_t sz = PyByteArray_Size(original);

    PyObject* copied = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copied)
        return NULL;

    if (sz)
        memcpy(PyByteArray_AS_STRING(copied), PyByteArray_AS_STRING(original), (size_t)sz);

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    return copied;
}

static MAYBE_INLINE PyObject* deepcopy_method_legacy(
    PyObject* original, PyObject* memo, PyObject** keepalive_pointer
) {
    PyObject* func = PyMethod_GET_FUNCTION(original);
    PyObject* self = PyMethod_GET_SELF(original);

    if (!func || !self)
        return NULL;

    Py_INCREF(self);
    PyObject* copied_self = deepcopy_legacy(self, memo, keepalive_pointer);
    Py_DECREF(self);

    if (!copied_self)
        return NULL;

    PyObject* copied = PyMethod_New(func, copied_self);
    Py_DECREF(copied_self);

    if (!copied)
        return NULL;

    if (memoize_legacy(memo, (void*)original, copied) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    if (keepalive_legacy(memo, keepalive_pointer, original) < 0) {
        Py_DECREF(copied);
        return NULL;
    }

    return copied;
}

static PyObject* reconstruct_newobj_legacy(
    PyObject* argtup, PyObject* memo, PyObject** keepalive_pointer
) {
    Py_ssize_t nargs = PyTuple_GET_SIZE(argtup);
    if (nargs < 1) {
        PyErr_SetString(PyExc_TypeError, "__newobj__ requires at least 1 argument");
        return NULL;
    }

    PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
    if (!PyType_Check(cls)) {
        PyErr_Format(
            PyExc_TypeError, "__newobj__ arg 1 must be a type, not %.200s", Py_TYPE(cls)->tp_name
        );
        return NULL;
    }

    PyObject* args = PyTuple_New(nargs - 1);
    if (!args)
        return NULL;

    for (Py_ssize_t i = 1; i < nargs; i++) {
        PyObject* arg = PyTuple_GET_ITEM(argtup, i);
        PyObject* copied_arg = deepcopy_legacy(arg, memo, keepalive_pointer);
        if (!copied_arg) {
            Py_DECREF(args);
            return NULL;
        }
        PyTuple_SET_ITEM(args, i - 1, copied_arg);
    }

    PyObject* instance = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, args, NULL);
    Py_DECREF(args);
    return instance;
}

static PyObject* reconstruct_newobj_ex_legacy(
    PyObject* argtup, PyObject* memo, PyObject** keepalive_pointer
) {
    if (PyTuple_GET_SIZE(argtup) != 3) {
        PyErr_Format(
            PyExc_TypeError, "__newobj_ex__ requires 3 arguments, got %zd", PyTuple_GET_SIZE(argtup)
        );
        return NULL;
    }

    PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
    PyObject* args = PyTuple_GET_ITEM(argtup, 1);
    PyObject* kwargs = PyTuple_GET_ITEM(argtup, 2);

    if (!PyType_Check(cls)) {
        PyErr_Format(
            PyExc_TypeError, "__newobj_ex__ arg 1 must be a type, not %.200s", Py_TYPE(cls)->tp_name
        );
        return NULL;
    }
    if (!PyTuple_Check(args)) {
        PyErr_Format(
            PyExc_TypeError,
            "__newobj_ex__ arg 2 must be a tuple, not %.200s",
            Py_TYPE(args)->tp_name
        );
        return NULL;
    }
    if (!PyDict_Check(kwargs)) {
        PyErr_Format(
            PyExc_TypeError,
            "__newobj_ex__ arg 3 must be a dict, not %.200s",
            Py_TYPE(kwargs)->tp_name
        );
        return NULL;
    }

    PyObject* copied_args = deepcopy_legacy(args, memo, keepalive_pointer);
    if (!copied_args)
        return NULL;

    PyObject* copied_kwargs = deepcopy_legacy(kwargs, memo, keepalive_pointer);
    if (!copied_kwargs) {
        Py_DECREF(copied_args);
        return NULL;
    }

    PyObject* instance = ((PyTypeObject*)cls)
                             ->tp_new((PyTypeObject*)cls, copied_args, copied_kwargs);
    Py_DECREF(copied_args);
    Py_DECREF(copied_kwargs);
    return instance;
}

static PyObject* reconstruct_callable_legacy(
    PyObject* callable, PyObject* argtup, PyObject* memo, PyObject** keepalive_pointer
) {
    Py_ssize_t nargs = PyTuple_GET_SIZE(argtup);

    if (nargs == 0)
        return PyObject_CallNoArgs(callable);

    PyObject* copied_args = PyTuple_New(nargs);
    if (!copied_args)
        return NULL;

    for (Py_ssize_t i = 0; i < nargs; i++) {
        PyObject* arg = PyTuple_GET_ITEM(argtup, i);
        PyObject* copied_arg = deepcopy_legacy(arg, memo, keepalive_pointer);
        if (!copied_arg) {
            Py_DECREF(copied_args);
            return NULL;
        }
        PyTuple_SET_ITEM(copied_args, i, copied_arg);
    }

    PyObject* instance = PyObject_CallObject(callable, copied_args);
    Py_DECREF(copied_args);
    return instance;
}

static int apply_setstate_legacy(
    PyObject* instance, PyObject* state, PyObject* memo, PyObject** keepalive_pointer
) {
    PyObject* setstate = NULL;
    if (PyObject_GetOptionalAttr(instance, module_state.s__setstate__, &setstate) < 0)
        return -1;

    if (!setstate)
        return 0;

    PyObject* copied_state = deepcopy_legacy(state, memo, keepalive_pointer);
    if (!copied_state) {
        Py_DECREF(setstate);
        return -1;
    }

    PyObject* result = PyObject_CallOneArg(setstate, copied_state);
    Py_DECREF(copied_state);
    Py_DECREF(setstate);

    if (!result)
        return -1;

    Py_DECREF(result);
    return 1;
}

static int apply_dict_state_legacy(
    PyObject* instance, PyObject* dict_state, PyObject* memo, PyObject** keepalive_pointer
) {
    if (!dict_state || dict_state == Py_None)
        return 0;

    if (!PyDict_Check(dict_state)) {
        PyErr_SetString(PyExc_TypeError, "state must be a dict");
        return -1;
    }

    PyObject* copied = deepcopy_legacy(dict_state, memo, keepalive_pointer);
    if (!copied)
        return -1;

    PyObject* instance_dict = PyObject_GetAttr(instance, module_state.s__dict__);
    if (!instance_dict) {
        Py_DECREF(copied);
        return -1;
    }

    PyObject *key, *value;
    Py_ssize_t pos = 0;
    int ret = 0;

    while (PyDict_Next(copied, &pos, &key, &value)) {
        if (PyObject_SetItem(instance_dict, key, value) < 0) {
            ret = -1;
            break;
        }
    }

    Py_DECREF(instance_dict);
    Py_DECREF(copied);
    return ret;
}

static int apply_slot_state_legacy(
    PyObject* instance, PyObject* slotstate, PyObject* memo, PyObject** keepalive_pointer
) {
    if (!slotstate || slotstate == Py_None)
        return 0;

    if (!PyDict_Check(slotstate)) {
        PyErr_SetString(PyExc_TypeError, "slot state is not a dictionary");
        return -1;
    }

    PyObject* copied = deepcopy_legacy(slotstate, memo, keepalive_pointer);
    if (!copied)
        return -1;

    PyObject *key, *value;
    Py_ssize_t pos = 0;
    int ret = 0;

    while (PyDict_Next(copied, &pos, &key, &value)) {
        if (PyObject_SetAttr(instance, key, value) < 0) {
            ret = -1;
            break;
        }
    }

    Py_DECREF(copied);
    return ret;
}

static int apply_state_tuple_legacy(
    PyObject* instance, PyObject* state, PyObject* memo, PyObject** keepalive_pointer
) {
    PyObject* dict_state = NULL;
    PyObject* slotstate = NULL;

    if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
        dict_state = PyTuple_GET_ITEM(state, 0);
        slotstate = PyTuple_GET_ITEM(state, 1);
    } else {
        dict_state = state;
    }

    if (apply_dict_state_legacy(instance, dict_state, memo, keepalive_pointer) < 0)
        return -1;

    if (apply_slot_state_legacy(instance, slotstate, memo, keepalive_pointer) < 0)
        return -1;

    return 0;
}

static int apply_listitems_legacy(
    PyObject* instance, PyObject* listitems, PyObject* memo, PyObject** keepalive_pointer
) {
    if (!listitems)
        return 0;

    PyObject* append = PyObject_GetAttr(instance, module_state.s_append);
    if (!append)
        return -1;

    PyObject* iterator = PyObject_GetIter(listitems);
    if (!iterator) {
        Py_DECREF(append);
        return -1;
    }

    int ret = 0;
    PyObject* item;

    while ((item = PyIter_Next(iterator))) {
        PyObject* copied_item = deepcopy_legacy(item, memo, keepalive_pointer);
        Py_DECREF(item);

        if (!copied_item) {
            ret = -1;
            break;
        }

        PyObject* result = PyObject_CallOneArg(append, copied_item);
        Py_DECREF(copied_item);

        if (!result) {
            ret = -1;
            break;
        }
        Py_DECREF(result);
    }

    if (ret == 0 && PyErr_Occurred())
        ret = -1;

    Py_DECREF(iterator);
    Py_DECREF(append);
    return ret;
}

static int apply_dictitems_legacy(
    PyObject* instance, PyObject* dictitems, PyObject* memo, PyObject** keepalive_pointer
) {
    if (!dictitems)
        return 0;

    PyObject* iterator = PyObject_GetIter(dictitems);
    if (!iterator)
        return -1;

    int ret = 0;
    PyObject* pair;

    while ((pair = PyIter_Next(iterator))) {
        if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
            Py_DECREF(pair);
            PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
            ret = -1;
            break;
        }

        PyObject* key = PyTuple_GET_ITEM(pair, 0);
        PyObject* value = PyTuple_GET_ITEM(pair, 1);
        Py_INCREF(key);
        Py_INCREF(value);
        Py_DECREF(pair);

        Py_SETREF(key, deepcopy_legacy(key, memo, keepalive_pointer));
        if (!key) {
            Py_DECREF(value);
            ret = -1;
            break;
        }

        Py_SETREF(value, deepcopy_legacy(value, memo, keepalive_pointer));
        if (!value) {
            Py_DECREF(key);
            ret = -1;
            break;
        }

        int status = PyObject_SetItem(instance, key, value);
        Py_DECREF(key);
        Py_DECREF(value);

        if (status < 0) {
            ret = -1;
            break;
        }
    }

    if (ret == 0 && PyErr_Occurred())
        ret = -1;

    Py_DECREF(iterator);
    return ret;
}

static PyObject* deepcopy_object_legacy(
    PyObject* original, PyTypeObject* tp, PyObject* memo, PyObject** keepalive_pointer
) {
    PyObject* reduce_result = try_reduce_via_registry(original, tp);
    if (!reduce_result) {
        if (PyErr_Occurred())
            return NULL;
        reduce_result = call_reduce_method_preferring_ex(original);
        if (!reduce_result)
            return NULL;
    }

    PyObject *callable, *argtup, *state, *listitems, *dictitems;
    int valid = validate_reduce_tuple(
        reduce_result, &callable, &argtup, &state, &listitems, &dictitems
    );

    if (valid == REDUCE_ERROR) {
        Py_DECREF(reduce_result);
        return NULL;
    }

    if (valid == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(original);
    }

    PyObject* instance;
    if (callable == module_state.copyreg___newobj__)
        instance = reconstruct_newobj_legacy(argtup, memo, keepalive_pointer);
    else if (callable == module_state.copyreg___newobj___ex)
        instance = reconstruct_newobj_ex_legacy(argtup, memo, keepalive_pointer);
    else
        instance = reconstruct_callable_legacy(callable, argtup, memo, keepalive_pointer);

    if (!instance) {
        Py_DECREF(reduce_result);
        return NULL;
    }

    if (memoize_legacy(memo, (void*)original, instance) < 0)
        goto error;

    if (state) {
        int applied = apply_setstate_legacy(instance, state, memo, keepalive_pointer);
        if (applied < 0)
            goto error;
        if (applied == 0 && apply_state_tuple_legacy(instance, state, memo, keepalive_pointer) < 0)
            goto error;
    }

    if (apply_listitems_legacy(instance, listitems, memo, keepalive_pointer) < 0)
        goto error;

    if (apply_dictitems_legacy(instance, dictitems, memo, keepalive_pointer) < 0)
        goto error;

    if (instance != original) {
        if (keepalive_legacy(memo, keepalive_pointer, original) < 0)
            goto error;
    }

    Py_DECREF(reduce_result);
    return instance;

error:
    Py_DECREF(reduce_result);
    Py_DECREF(instance);
    return NULL;
}

#endif