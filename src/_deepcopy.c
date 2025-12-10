/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_DEEPCOPY_C
#define _COPIUM_DEEPCOPY_C

#include "_memo.c"
#include "_dict_iter.c"
#include "_type_checks.c"
#include "_recursion_guard.c"
#include "_reduce_helpers.c"
#include "_fallback.c"

#include "object.h"

#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
    #include "pycore_setobject.h"
#endif

static MAYBE_INLINE PyObject* deepcopy_list(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_tuple(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_dict(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_set(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_frozenset(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_bytearray(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_method(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static MAYBE_INLINE PyObject* deepcopy_custom(
    PyObject* original, PyObject* __deepcopy__, PyMemoObject* memo, Py_ssize_t memo_key_hash
);
static PyObject* deepcopy_object(
    PyObject* original, PyTypeObject* tp, PyMemoObject* memo, Py_ssize_t memo_key_hash
);

static ALWAYS_INLINE PyObject* deepcopy(PyObject* original, PyMemoObject* memo) {
    assert(memo != NULL && "deepcopy: memo must not be NULL");

    PyTypeObject* original_type = Py_TYPE(original);

    /* Literal immutables */
    if (LIKELY(is_literal_immutable(original_type))) {
        return Py_NewRef(original);
    }

    Py_ssize_t memo_key_hash;
    PyObject* memoized = remember(memo, original, &memo_key_hash);
    if (memoized)
        return memoized;

    /* Popular builtin containers */
    if (original_type == &PyTuple_Type)
        return RECURSION_GUARDED(deepcopy_tuple(original, memo, memo_key_hash));
    if (original_type == &PyDict_Type)
        return RECURSION_GUARDED(deepcopy_dict(original, memo, memo_key_hash));
    if (original_type == &PyList_Type)
        return RECURSION_GUARDED(deepcopy_list(original, memo, memo_key_hash));
    if (original_type == &PySet_Type)
        return RECURSION_GUARDED(deepcopy_set(original, memo, memo_key_hash));

    /* Less likely to occur atomic immutables */
    if (is_builtin_immutable(original_type) || is_class(original_type)) {
        return Py_NewRef(original);
    }

    /* Less likely to occur builtin */
    if (original_type == &PyFrozenSet_Type)
        return RECURSION_GUARDED(deepcopy_frozenset(original, memo, memo_key_hash));
    if (original_type == &PyByteArray_Type)
        return deepcopy_bytearray(original, memo, memo_key_hash);
    if (original_type == &PyMethod_Type)
        return deepcopy_method(original, memo, memo_key_hash);

    /* Non-static stdlib immutables */
    if (is_stdlib_immutable(original_type))
        return Py_NewRef(original);

    {
        PyObject* __deepcopy__ = NULL;
        int has_deepcopy = PyObject_GetOptionalAttr(
            original, module_state.s__deepcopy__, &__deepcopy__
        );
        if (has_deepcopy < 0)
            return NULL;
        if (has_deepcopy) {
            return deepcopy_custom(original, __deepcopy__, memo, memo_key_hash);
        }
    }

    return deepcopy_object(original, original_type, memo, memo_key_hash);
}

static MAYBE_INLINE PyObject* deepcopy_list(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied = NULL;
    PyObject* copied_item = NULL;

    Py_ssize_t sz = Py_SIZE(original);  // TODO: use PyList_GET_SIZE
    copied = PyList_New(sz);
    if (!copied)
        goto error;

    // Once we put list in memo, Python will be able access its items,
    // which will lead to segfault if we won't override NULL pointers
    // with a valid PyObject. Still this is much faster than using PyList_Append.
    for (Py_ssize_t i = 0; i < sz; ++i) {
        Py_INCREF(Py_None);                   // TODO: drop this for 3.12+
        PyList_SET_ITEM(copied, i, Py_None);  // TODO: use Py_Ellipsis
    }
    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        // TODO: check size before using GET_ITEM, original list could change
        // TODO: maybe we should Py_INCREF(item)?
        PyObject* item = PyList_GET_ITEM(original, i);
        copied_item = deepcopy(item, memo);
        if (!copied_item)
            goto memoized_error;
        Py_DECREF(Py_None);
        PyList_SET_ITEM(copied, i, copied_item);
    }
    // TODO: merge these two loops, or forbid list size changing
    //  stdlib allows that, but I doubt anybody relies on that.
    Py_ssize_t i = sz;
    while (i < Py_SIZE(original)) {
        PyObject* item = PyList_GET_ITEM(original, i);
        copied_item = deepcopy(item, memo);
        if (!copied_item)
            goto memoized_error;
        if (PyList_Append(copied, copied_item) < 0)
            goto memoized_error;
        Py_DECREF(copied_item);
        copied_item = NULL;
        i++;
    }
    return copied;

memoized_error:
    forget(memo, original, memo_key_hash);
error:
    Py_XDECREF(copied);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_tuple(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied = NULL;
    PyObject* copied_item = NULL;

    Py_ssize_t sz = Py_SIZE(original);
    copied = PyTuple_New(sz);
    if (!copied)
        goto error;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyTuple_GET_ITEM(original, i);
        copied_item = deepcopy(item, memo);
        if (!copied_item)
            goto error;
        if (copied_item != item)
            all_same = 0;
        PyTuple_SET_ITEM(copied, i, copied_item);
        copied_item = NULL;
    }
    if (all_same) {
        Py_DECREF(copied);
        return Py_NewRef(original);
    }

    /* Self-referential tuples: recursive path may have already memoized a copy */
    PyObject* existing = memo_table_lookup_h(memo->table, (void*)original, memo_key_hash);
    if (existing) {
        Py_DECREF(copied);
        return Py_NewRef(existing);
    }

    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;
    return copied;

error:
    Py_XDECREF(copied);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_dict(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied = NULL;
    PyObject* copied_key = NULL;
    PyObject* copied_value = NULL;

    Py_ssize_t sz = Py_SIZE(original);  // TODO: this is wrong, use PyDict_Size
    copied = _PyDict_NewPresized(sz);
    if (!copied)
        goto error_no_cleanup;
    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error_no_cleanup;

    DictIterGuard iter_guard;
    dict_iter_init(&iter_guard, original);

    PyObject *key, *value;
    int iter_flag;
    while ((iter_flag = dict_iter_next(&iter_guard, &key, &value)) > 0) {
        copied_key = deepcopy(key, memo);
        if (!copied_key)
            goto memoized_error;
        copied_value = deepcopy(value, memo);
        if (!copied_value)
            goto memoized_error;
        if (PyDict_SetItem(copied, copied_key, copied_value) < 0)
            goto memoized_error;
        Py_DECREF(copied_key);
        copied_key = NULL;
        Py_DECREF(copied_value);
        copied_value = NULL;
    }
    // TODO: check within cycle
    if (iter_flag < 0)
        goto memoized_error_no_cleanup;

    return copied;

memoized_error:
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    dict_iter_cleanup(&iter_guard);
#endif
memoized_error_no_cleanup:
    forget(memo, original, memo_key_hash);
    goto error_no_cleanup;
error_no_cleanup:
    Py_XDECREF(copied);
    Py_XDECREF(copied_key);
    Py_XDECREF(copied_value);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_set(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied = NULL;
    PyObject* snapshot = NULL;
    PyObject* copied_item = NULL;

    copied = PySet_New(NULL);
    if (!copied)
        goto error;
    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;

    // Original deepcopy used __reduce__ protocol to copy set, which prevented case
    // when set size could change mid iteration. Preserving this behavior to the best of our ability.
    // TODO: lock the set on free-threaded builds
    Py_ssize_t n = PySet_Size(original);
    if (n == -1)
        goto memoized_error;
    snapshot = PyTuple_New(n);
    if (!snapshot)
        goto memoized_error;

    Py_ssize_t pos = 0;
    PyObject* item;
    Py_hash_t hash;
    Py_ssize_t i = 0;

    while (_PySet_NextEntry(original, &pos, &item, &hash)) {
        if (i < n) {
            Py_INCREF(item);
            PyTuple_SET_ITEM(snapshot, i, item);
            i++;
        }
    }

    for (Py_ssize_t j = 0; j < i; j++) {
        PyObject* element = PyTuple_GET_ITEM(snapshot, j);
        copied_item = deepcopy(element, memo);
        if (!copied_item)
            goto memoized_error;
        if (PySet_Add(copied, copied_item) < 0)
            goto memoized_error;
        Py_DECREF(copied_item);
        copied_item = NULL;
    }
    Py_DECREF(snapshot);

    return copied;

memoized_error:
    forget(memo, original, memo_key_hash);
error:
    Py_XDECREF(copied);
    Py_XDECREF(snapshot);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied_items = NULL;
    PyObject* copied = NULL;
    PyObject* copied_item = NULL;

    Py_ssize_t n = PySet_Size(original);
    if (n == -1)
        goto error;

    copied_items = PyTuple_New(n);
    if (!copied_items)
        goto error;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(original, &pos, &item, &hash)) {
        copied_item = deepcopy(item, memo);
        if (!copied_item)
            goto error;
        PyTuple_SET_ITEM(copied_items, i, copied_item);
        copied_item = NULL;
        i++;
    }

    copied = PyFrozenSet_New(copied_items);
    Py_DECREF(copied_items);
    copied_items = NULL;
    if (!copied)
        goto error;
    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;
    return copied;

error:
    Py_XDECREF(copied_items);
    Py_XDECREF(copied);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_bytearray(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied = NULL;

    Py_ssize_t sz = PyByteArray_Size(original);
    copied = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copied)
        goto error;
    if (sz)
        memcpy(PyByteArray_AS_STRING(copied), PyByteArray_AS_STRING(original), (size_t)sz);
    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;
    return copied;

error:
    Py_XDECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_method(
    PyObject* original, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* func = NULL;
    PyObject* self = NULL;
    PyObject* copied_self = NULL;
    PyObject* copied = NULL;

    func = PyMethod_GET_FUNCTION(original);
    self = PyMethod_GET_SELF(original);
    if (!func || !self)
        goto error;

    Py_INCREF(func);
    Py_INCREF(self);
    copied_self = deepcopy(self, memo);
    Py_DECREF(self);
    self = NULL;
    if (!copied_self)
        goto error;

    copied = PyMethod_New(func, copied_self);
    Py_DECREF(func);
    func = NULL;
    Py_DECREF(copied_self);
    copied_self = NULL;
    if (!copied)
        goto error;

    if (memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;
    return copied;

error:
    Py_XDECREF(func);
    Py_XDECREF(self);
    Py_XDECREF(copied_self);
    Py_XDECREF(copied);
    return NULL;
}

static PyObject* deepcopy_custom(
    PyObject* original, PyObject* __deepcopy__, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* copied = NULL;

    MemoCheckpoint checkpoint = memo_checkpoint(memo);

    copied = PyObject_CallOneArg(__deepcopy__, (PyObject*)memo);

    if (!copied)
        copied = _maybe_retry_with_dict_memo(original, __deepcopy__, memo, checkpoint);

    if (!copied)
        goto done;

    if (copied != original && memoize(memo, original, copied, memo_key_hash) < 0)
        goto error;

    goto done;

error:
    Py_CLEAR(copied);
done:
    Py_DECREF(__deepcopy__);
    return copied;
}

static PyObject* deepcopy_object(
    PyObject* original, PyTypeObject* tp, PyMemoObject* memo, Py_ssize_t memo_key_hash
) {
    PyObject* reduce_result = NULL;
    PyObject* instance = NULL;
    PyObject* setstate = NULL;
    PyObject* copied_state = NULL;
    PyObject* call_result = NULL;
    PyObject* copied_dict_state = NULL;
    PyObject* instance_dict = NULL;
    PyObject* copied_slotstate = NULL;
    PyObject* append = NULL;
    PyObject* iterator = NULL;
    PyObject* copied_item = NULL;
    PyObject* copied_key = NULL;
    PyObject* copied_value = NULL;

    reduce_result = try_reduce_via_registry(original, tp);
    if (!reduce_result) {
        if (PyErr_Occurred())
            goto error;
        reduce_result = call_reduce_method_preferring_ex(original);
        if (!reduce_result)
            goto error;
    }

    PyObject *callable, *argtup, *state, *listitems, *dictitems;
    int valid = validate_reduce_tuple(
        reduce_result, &callable, &argtup, &state, &listitems, &dictitems
    );
    if (valid == REDUCE_ERROR)
        goto error;
    if (valid == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(original);
    }

    if (callable == module_state.copyreg___newobj__) {
        if (PyTuple_GET_SIZE(argtup) < 1) {
            PyErr_Format(
                PyExc_TypeError,
                "__newobj__ expected at least 1 argument, got %zd",
                PyTuple_GET_SIZE(argtup)
            );
            goto error;
        }

        PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
        if (!PyType_Check(cls)) {
            PyErr_Format(
                PyExc_TypeError,
                "first argument to __newobj__() must be a class, not %.200s",
                Py_TYPE(cls)->tp_name
            );
            goto error;
        }

        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup) - 1;
        PyObject* copied_args = PyTuple_New(nargs);
        if (!copied_args)
            goto error;

        for (Py_ssize_t i = 0; i < nargs; i++) {
            PyObject* arg = PyTuple_GET_ITEM(argtup, i + 1);
            PyObject* copied_arg = deepcopy(arg, memo);
            if (!copied_arg) {
                Py_DECREF(copied_args);
                goto error;
            }
            PyTuple_SET_ITEM(copied_args, i, copied_arg);
        }

        instance = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, copied_args, NULL);
        Py_DECREF(copied_args);
        if (!instance)
            goto error;
    } else if (callable == module_state.copyreg___newobj___ex) {
        if (PyTuple_GET_SIZE(argtup) != 3) {
            PyErr_Format(
                PyExc_TypeError,
                "__newobj_ex__ expected 3 arguments, got %zd",
                PyTuple_GET_SIZE(argtup)
            );
            goto error;
        }

        PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
        PyObject* args = PyTuple_GET_ITEM(argtup, 1);
        PyObject* kwargs = PyTuple_GET_ITEM(argtup, 2);

        if (!PyType_Check(cls)) {
            PyErr_Format(
                PyExc_TypeError,
                "first argument to __newobj_ex__() must be a class, not %.200s",
                Py_TYPE(cls)->tp_name
            );
            goto error;
        }
        if (!PyTuple_Check(args)) {
            PyErr_Format(
                PyExc_TypeError,
                "second argument to __newobj_ex__() must be a tuple, not %.200s",
                Py_TYPE(args)->tp_name
            );
            goto error;
        }
        if (!PyDict_Check(kwargs)) {
            PyErr_Format(
                PyExc_TypeError,
                "third argument to __newobj_ex__() must be a dict, not %.200s",
                Py_TYPE(kwargs)->tp_name
            );
            goto error;
        }

        PyObject* copied_args = deepcopy(args, memo);
        if (!copied_args)
            goto error;

        PyObject* copied_kwargs = deepcopy(kwargs, memo);
        if (!copied_kwargs) {
            Py_DECREF(copied_args);
            goto error;
        }

        instance = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, copied_args, copied_kwargs);
        Py_DECREF(copied_args);
        Py_DECREF(copied_kwargs);
        if (!instance)
            goto error;
    } else {
        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup);
        if (nargs == 0) {
            instance = PyObject_CallNoArgs(callable);
        } else {
            PyObject* copied_argtup = PyTuple_New(nargs);
            if (!copied_argtup)
                goto error;

            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject* arg = PyTuple_GET_ITEM(argtup, i);
                PyObject* copied_arg = deepcopy(arg, memo);
                if (!copied_arg) {
                    Py_DECREF(copied_argtup);
                    goto error;
                }
                PyTuple_SET_ITEM(copied_argtup, i, copied_arg);
            }

            instance = PyObject_CallObject(callable, copied_argtup);
            Py_DECREF(copied_argtup);
        }
        if (!instance)
            goto error;
    }

    if (memoize(memo, original, instance, memo_key_hash) < 0)
        goto error;

    if (state) {
        if (PyObject_GetOptionalAttr(instance, module_state.s__setstate__, &setstate) < 0)
            goto memoized_error;

        if (setstate) {
            copied_state = deepcopy(state, memo);
            if (!copied_state) {
                Py_DECREF(setstate);
                setstate = NULL;
                goto memoized_error;
            }

            call_result = PyObject_CallOneArg(setstate, copied_state);
            Py_DECREF(copied_state);
            copied_state = NULL;
            Py_DECREF(setstate);
            setstate = NULL;
            if (!call_result)
                goto memoized_error;
            Py_DECREF(call_result);
            call_result = NULL;
        } else {
            PyObject* dict_state = NULL;
            PyObject* slotstate = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slotstate = PyTuple_GET_ITEM(state, 1);
            } else {
                dict_state = state;
            }

            if (dict_state && dict_state != Py_None) {
                if (!PyDict_Check(dict_state)) {
                    PyErr_SetString(PyExc_TypeError, "state is not a dictionary");
                    goto memoized_error;
                }

                copied_dict_state = deepcopy(dict_state, memo);
                if (!copied_dict_state)
                    goto memoized_error;

                instance_dict = PyObject_GetAttr(instance, module_state.s__dict__);
                if (!instance_dict) {
                    Py_DECREF(copied_dict_state);
                    copied_dict_state = NULL;
                    goto memoized_error;
                }

                PyObject *key, *value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(copied_dict_state, &pos, &key, &value)) {
                    if (PyObject_SetItem(instance_dict, key, value) < 0) {
                        Py_DECREF(instance_dict);
                        instance_dict = NULL;
                        Py_DECREF(copied_dict_state);
                        copied_dict_state = NULL;
                        goto memoized_error;
                    }
                }

                Py_DECREF(instance_dict);
                instance_dict = NULL;
                Py_DECREF(copied_dict_state);
                copied_dict_state = NULL;
            }

            if (slotstate && slotstate != Py_None) {
                if (!PyDict_Check(slotstate)) {
                    PyErr_SetString(PyExc_TypeError, "slot state is not a dictionary");
                    goto memoized_error;
                }

                copied_slotstate = deepcopy(slotstate, memo);
                if (!copied_slotstate)
                    goto memoized_error;

                PyObject *key, *value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(copied_slotstate, &pos, &key, &value)) {
                    if (PyObject_SetAttr(instance, key, value) < 0) {
                        Py_DECREF(copied_slotstate);
                        copied_slotstate = NULL;
                        goto memoized_error;
                    }
                }

                Py_DECREF(copied_slotstate);
                copied_slotstate = NULL;
            }
        }
    }

    if (listitems) {
        append = PyObject_GetAttr(instance, module_state.s_append);
        if (!append)
            goto memoized_error;

        iterator = PyObject_GetIter(listitems);
        if (!iterator) {
            Py_DECREF(append);
            append = NULL;
            goto memoized_error;
        }

        PyObject* item;
        while ((item = PyIter_Next(iterator)) != NULL) {
            copied_item = deepcopy(item, memo);
            Py_DECREF(item);
            if (!copied_item) {
                Py_DECREF(iterator);
                iterator = NULL;
                Py_DECREF(append);
                append = NULL;
                goto memoized_error;
            }

            call_result = PyObject_CallOneArg(append, copied_item);
            Py_DECREF(copied_item);
            copied_item = NULL;
            if (!call_result) {
                Py_DECREF(iterator);
                iterator = NULL;
                Py_DECREF(append);
                append = NULL;
                goto memoized_error;
            }
            Py_DECREF(call_result);
            call_result = NULL;
        }

        if (PyErr_Occurred()) {
            Py_DECREF(iterator);
            iterator = NULL;
            Py_DECREF(append);
            append = NULL;
            goto memoized_error;
        }

        Py_DECREF(iterator);
        iterator = NULL;
        Py_DECREF(append);
        append = NULL;
    }

    if (dictitems) {
        iterator = PyObject_GetIter(dictitems);
        if (!iterator)
            goto memoized_error;

        PyObject* pair;
        while ((pair = PyIter_Next(iterator)) != NULL) {
            if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
                Py_DECREF(pair);
                Py_DECREF(iterator);
                iterator = NULL;
                PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
                goto memoized_error;
            }

            PyObject* key = PyTuple_GET_ITEM(pair, 0);
            PyObject* value = PyTuple_GET_ITEM(pair, 1);

            copied_key = deepcopy(key, memo);
            if (!copied_key) {
                Py_DECREF(pair);
                Py_DECREF(iterator);
                iterator = NULL;
                goto memoized_error;
            }

            copied_value = deepcopy(value, memo);
            if (!copied_value) {
                Py_DECREF(pair);
                Py_DECREF(iterator);
                iterator = NULL;
                goto memoized_error;
            }

            Py_DECREF(pair);

            int status = PyObject_SetItem(instance, copied_key, copied_value);
            Py_DECREF(copied_key);
            copied_key = NULL;
            Py_DECREF(copied_value);
            copied_value = NULL;

            if (status < 0) {
                Py_DECREF(iterator);
                iterator = NULL;
                goto memoized_error;
            }
        }

        if (PyErr_Occurred()) {
            Py_DECREF(iterator);
            iterator = NULL;
            goto memoized_error;
        }

        Py_DECREF(iterator);
        iterator = NULL;
    }

    Py_DECREF(reduce_result);
    return instance;

memoized_error:
    forget(memo, original, memo_key_hash);
error:
    Py_XDECREF(instance);
    Py_XDECREF(reduce_result);
    Py_XDECREF(setstate);
    Py_XDECREF(copied_state);
    Py_XDECREF(call_result);
    Py_XDECREF(copied_dict_state);
    Py_XDECREF(instance_dict);
    Py_XDECREF(copied_slotstate);
    Py_XDECREF(append);
    Py_XDECREF(iterator);
    Py_XDECREF(copied_item);
    Py_XDECREF(copied_key);
    Py_XDECREF(copied_value);
    return NULL;
}
#endif