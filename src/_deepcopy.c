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

/* _PySet_NextEntry() */
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
    #include "pycore_setobject.h"
#endif

static MAYBE_INLINE PyObject* deepcopy_list(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_tuple(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_dict(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_set(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_frozenset(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_bytearray(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_method(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static PyObject* deepcopy_object(
    PyObject* obj, PyTypeObject* tp, MemoObject* memo, Py_ssize_t id_hash
);

static ALWAYS_INLINE PyObject* deepcopy(PyObject* obj, MemoObject* memo) {
    assert(memo != NULL && "deepcopy_c: memo must not be NULL");

    PyTypeObject* tp = Py_TYPE(obj);
    /* 1) Immortal or literal immutables → fastest return */
    if (LIKELY(is_literal_immutable(tp))) {
        return Py_NewRef(obj);
    }

    /* 2) Memo hit */
    void* id = (void*)obj;
    Py_ssize_t h = memo_hash_pointer(id);
    PyObject* hit = memo_table_lookup_h(memo->table, id, h);
    if (hit)
        return Py_NewRef(hit);

    /* 3) Popular containers first (specialized, likely hot) */
    if (tp == &PyDict_Type)
        return RECURSION_GUARDED(deepcopy_dict(obj, memo, h));
    if (tp == &PyList_Type)
        return RECURSION_GUARDED(deepcopy_list(obj, memo, h));
    if (tp == &PyTuple_Type)
        return RECURSION_GUARDED(deepcopy_tuple(obj, memo, h));
    if (tp == &PySet_Type)
        return RECURSION_GUARDED(deepcopy_set(obj, memo, h));

    /* 4) Other atomic immutables (builtin/class types) */
    if (is_builtin_immutable(tp) || is_class(tp)) {
        return Py_NewRef(obj);
    }

    if (tp == &PyFrozenSet_Type)
        return deepcopy_frozenset(obj, memo, h);

    if (tp == &PyByteArray_Type)
        return deepcopy_bytearray(obj, memo, h);

    if (tp == &PyMethod_Type)
        return deepcopy_method(obj, memo, h);

    if (is_stdlib_immutable(tp))  // touch non-static types last
        return Py_NewRef(obj);

    /* Robustly detect a user-defined __deepcopy__ via optional lookup (single step, non-raising on miss). */
    {
        PyObject* deepcopy_meth = NULL;
        int has_deepcopy = PyObject_GetOptionalAttr(obj, module_state.str_deepcopy, &deepcopy_meth);
        if (has_deepcopy < 0)
            return NULL;
        if (has_deepcopy) {
            PyObject* res = PyObject_CallOneArg(deepcopy_meth, (PyObject*)memo);
            Py_DECREF(deepcopy_meth);
            if (!res)
                return NULL;
            if (res != obj) {
                if (memo_table_insert_h(&memo->table, (void*)obj, res, h) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
                if (keepalive_append(&memo->keepalive, obj) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
            }
            return res;
        }
    }

    PyObject* res = deepcopy_object(obj, tp, memo, h);
    return res;
}

static MAYBE_INLINE PyObject* deepcopy_list(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* copied_item = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = PyList_New(sz);
    if (!copy)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyList_SET_ITEM(copy, i, Py_NewRef(Py_None));
    }
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyList_GET_ITEM(obj, i);
        copied_item = deepcopy(item, memo);
        if (!copied_item)
            goto error;
        Py_DECREF(Py_None);
        PyList_SET_ITEM(copy, i, copied_item);
        copied_item = NULL;  // ownership transferred to list
    }

    /* If the source list grew during the first pass, append the extra tail items. */
    Py_ssize_t i2 = sz;
    while (i2 < Py_SIZE(obj)) {
        PyObject* item2 = PyList_GET_ITEM(obj, i2);
        copied_item = deepcopy(item2, memo);
        if (!copied_item)
            goto error;
        if (PyList_Append(copy, copied_item) < 0)
            goto error;
        Py_DECREF(copied_item);
        copied_item = NULL;
        i2++;
    }
    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_tuple(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* copied = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = PyTuple_New(sz);
    if (!copy)
        goto error;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyTuple_GET_ITEM(obj, i);
        copied = deepcopy(item, memo);
        if (!copied)
            goto error;
        if (copied != item)
            all_same = 0;
        PyTuple_SET_ITEM(copy, i, copied);
        copied = NULL;  // ownership transferred to tuple
    }
    if (all_same) {
        Py_DECREF(copy);
        return Py_NewRef(obj);
    }

    /* Handle self-referential tuples: if a recursive path already created a copy,
       prefer that existing copy to maintain identity. */
    PyObject* existing = memo_table_lookup_h(memo->table, (void*)obj, id_hash);
    if (existing) {
        Py_DECREF(copy);
        return Py_NewRef(existing);
    }

    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_dict(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* ckey = NULL;
    PyObject* cvalue = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = _PyDict_NewPresized(sz);
    if (!copy)
        goto error_no_cleanup;
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error_no_cleanup;

    // NOTE: iter_guard is declared here, after early returns.
    // error_no_cleanup is for paths before iter_guard is initialized or after dict_iter_next
    // already cleaned up. error is for mid-iteration cleanup (3.14+ only).
    DictIterGuard iter_guard;
    dict_iter_init(&iter_guard, obj);

    PyObject *key, *value;
    int ret;
    while ((ret = dict_iter_next(&iter_guard, &key, &value)) > 0) {
        ckey = deepcopy(key, memo);
        if (!ckey)
            goto error;
        cvalue = deepcopy(value, memo);
        if (!cvalue)
            goto error;
        if (PyDict_SetItem(copy, ckey, cvalue) < 0)
            goto error;
        Py_DECREF(ckey);
        ckey = NULL;
        Py_DECREF(cvalue);
        cvalue = NULL;
    }
    if (ret < 0)
        goto error_no_cleanup;  // dict_iter_next already cleaned up on -1

    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    dict_iter_cleanup(&iter_guard);
#endif
error_no_cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(ckey);
    Py_XDECREF(cvalue);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_set(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* snap = NULL;
    PyObject* citem = NULL;

    copy = PySet_New(NULL);
    if (!copy)
        goto error;
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;

    /* Snapshot into a pre-sized tuple without invoking user code. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        goto error;
    snap = PyTuple_New(n);
    if (!snap)
        goto error;

    Py_ssize_t pos = 0;
    PyObject* item;
    Py_hash_t hash;
    Py_ssize_t i = 0;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        if (i < n) {
            /* item is borrowed; store owned ref in the tuple */
            Py_INCREF(item);
            PyTuple_SET_ITEM(snap, i, item);
            i++;
        } else {
            /* If the set grows during snapshotting, ignore extras to avoid overflow. */
        }
    }

    for (Py_ssize_t j = 0; j < i; j++) {
        PyObject* elem = PyTuple_GET_ITEM(snap, j); /* borrowed from snapshot */
        citem = deepcopy(elem, memo);
        if (!citem)
            goto error;
        if (PySet_Add(copy, citem) < 0)
            goto error;
        Py_DECREF(citem);
        citem = NULL;
    }
    Py_DECREF(snap);

    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(snap);
    Py_XDECREF(citem);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* temp = NULL;
    PyObject* copy = NULL;
    PyObject* citem = NULL;

    /* Pre-size snapshot: frozenset is immutable, so size won't change mid-loop. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        goto error;

    temp = PyTuple_New(n);
    if (!temp)
        goto error;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        /* item is borrowed; deepcopy_c returns a new reference which tuple will own. */
        citem = deepcopy(item, memo);
        if (!citem)
            goto error;
        PyTuple_SET_ITEM(temp, i, citem);  // steals reference to citem
        citem = NULL;                      // ownership transferred
        i++;
    }

    copy = PyFrozenSet_New(temp);
    Py_DECREF(temp);
    temp = NULL;
    if (!copy)
        goto error;
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(temp);
    Py_XDECREF(copy);
    Py_XDECREF(citem);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_bytearray(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
) {
    PyObject* copy = NULL;

    Py_ssize_t sz = PyByteArray_Size(obj);
    copy = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copy)
        goto error;
    if (sz)
        memcpy(PyByteArray_AS_STRING(copy), PyByteArray_AS_STRING(obj), (size_t)sz);
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_method(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* func = NULL;
    PyObject* self = NULL;
    PyObject* cself = NULL;
    PyObject* copy = NULL;

    func = PyMethod_GET_FUNCTION(obj);
    self = PyMethod_GET_SELF(obj);
    if (!func || !self)
        goto error;

    Py_INCREF(func);
    Py_INCREF(self);
    cself = deepcopy(self, memo);
    Py_DECREF(self);
    self = NULL;
    if (!cself)
        goto error;

    copy = PyMethod_New(func, cself);
    Py_DECREF(func);
    func = NULL;
    Py_DECREF(cself);
    cself = NULL;
    if (!copy)
        goto error;

    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (keepalive_append(&memo->keepalive, obj) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(func);
    Py_XDECREF(self);
    Py_XDECREF(cself);
    Py_XDECREF(copy);
    return NULL;
}

// Reduce protocol implementation for C-memo path.
// This is the cold fallback path - not marked ALWAYS_INLINE to avoid code bloat.
static PyObject* deepcopy_object(
    PyObject* obj, PyTypeObject* tp, MemoObject* memo, Py_ssize_t id_hash
) {
    // All owned refs declared at top for cleanup
    PyObject* reduce_result = NULL;
    PyObject* inst = NULL;
    PyObject* setstate = NULL;
    PyObject* state_copy = NULL;
    PyObject* result = NULL;
    PyObject* dict_state_copy = NULL;
    PyObject* inst_dict = NULL;
    PyObject* slotstate_copy = NULL;
    PyObject* append = NULL;
    PyObject* it = NULL;
    PyObject* item_copy = NULL;
    PyObject* key_copy = NULL;
    PyObject* value_copy = NULL;

    reduce_result = try_reduce_via_registry(obj, tp);
    if (!reduce_result) {
        if (PyErr_Occurred())
            goto error;
        reduce_result = call_reduce_method_preferring_ex(obj);
        if (!reduce_result)
            goto error;
    }

    PyObject *callable, *argtup, *state, *listitems, *dictitems;
    int valid =
        validate_reduce_tuple(reduce_result, &callable, &argtup, &state, &listitems, &dictitems);
    if (valid == REDUCE_ERROR)
        goto error;
    if (valid == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(obj);
    }

    // Handle __newobj__
    if (callable == module_state.copyreg_newobj) {
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
        PyObject* newargs = PyTuple_New(nargs);
        if (!newargs)
            goto error;

        for (Py_ssize_t i = 0; i < nargs; i++) {
            PyObject* arg = PyTuple_GET_ITEM(argtup, i + 1);
            PyObject* arg_copy = deepcopy(arg, memo);
            if (!arg_copy) {
                Py_DECREF(newargs);
                goto error;
            }
            PyTuple_SET_ITEM(newargs, i, arg_copy);
        }

        inst = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, newargs, NULL);
        Py_DECREF(newargs);
        if (!inst)
            goto error;
    }
    // Handle __newobj_ex__
    else if (callable == module_state.copyreg_newobj_ex) {
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

        PyObject* args_copy = deepcopy(args, memo);
        if (!args_copy)
            goto error;

        PyObject* kwargs_copy = deepcopy(kwargs, memo);
        if (!kwargs_copy) {
            Py_DECREF(args_copy);
            goto error;
        }

        inst = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, args_copy, kwargs_copy);
        Py_DECREF(args_copy);
        Py_DECREF(kwargs_copy);
        if (!inst)
            goto error;
    }
    // Generic callable
    else {
        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup);
        if (nargs == 0) {
            inst = PyObject_CallNoArgs(callable);
        } else {
            PyObject* argtup_copy = PyTuple_New(nargs);
            if (!argtup_copy)
                goto error;

            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject* arg = PyTuple_GET_ITEM(argtup, i);
                PyObject* arg_copy = deepcopy(arg, memo);
                if (!arg_copy) {
                    Py_DECREF(argtup_copy);
                    goto error;
                }
                PyTuple_SET_ITEM(argtup_copy, i, arg_copy);
            }

            inst = PyObject_CallObject(callable, argtup_copy);
            Py_DECREF(argtup_copy);
        }
        if (!inst)
            goto error;
    }

    // Memoize early to handle self-referential structures
    if (memo_table_insert_h(&memo->table, (void*)obj, inst, id_hash) < 0)
        goto error;

    // Handle state (BUILD semantics)
    if (state) {
        if (PyObject_GetOptionalAttr(inst, module_state.str_setstate, &setstate) < 0)
            goto error;

        if (setstate) {
            // Explicit __setstate__
            state_copy = deepcopy(state, memo);
            if (!state_copy) {
                Py_DECREF(setstate);
                setstate = NULL;
                goto error;
            }

            result = PyObject_CallOneArg(setstate, state_copy);
            Py_DECREF(state_copy);
            state_copy = NULL;
            Py_DECREF(setstate);
            setstate = NULL;
            if (!result)
                goto error;
            Py_DECREF(result);
            result = NULL;
        } else {
            // Default __setstate__: handle inst.__dict__ and slotstate
            PyObject* dict_state = NULL;
            PyObject* slotstate = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slotstate = PyTuple_GET_ITEM(state, 1);
            } else {
                dict_state = state;
            }

            // Set inst.__dict__ from the state dict
            if (dict_state && dict_state != Py_None) {
                if (!PyDict_Check(dict_state)) {
                    PyErr_SetString(PyExc_TypeError, "state is not a dictionary");
                    goto error;
                }

                dict_state_copy = deepcopy(dict_state, memo);
                if (!dict_state_copy)
                    goto error;

                inst_dict = PyObject_GetAttr(inst, module_state.str_dict);
                if (!inst_dict) {
                    Py_DECREF(dict_state_copy);
                    dict_state_copy = NULL;
                    goto error;
                }

                PyObject *d_key, *d_value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(dict_state_copy, &pos, &d_key, &d_value)) {
                    if (PyObject_SetItem(inst_dict, d_key, d_value) < 0) {
                        Py_DECREF(inst_dict);
                        inst_dict = NULL;
                        Py_DECREF(dict_state_copy);
                        dict_state_copy = NULL;
                        goto error;
                    }
                }

                Py_DECREF(inst_dict);
                inst_dict = NULL;
                Py_DECREF(dict_state_copy);
                dict_state_copy = NULL;
            }

            // Also set instance attributes from the slotstate dict
            if (slotstate && slotstate != Py_None) {
                if (!PyDict_Check(slotstate)) {
                    PyErr_SetString(PyExc_TypeError, "slot state is not a dictionary");
                    goto error;
                }

                slotstate_copy = deepcopy(slotstate, memo);
                if (!slotstate_copy)
                    goto error;

                PyObject *d_key, *d_value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(slotstate_copy, &pos, &d_key, &d_value)) {
                    if (PyObject_SetAttr(inst, d_key, d_value) < 0) {
                        Py_DECREF(slotstate_copy);
                        slotstate_copy = NULL;
                        goto error;
                    }
                }

                Py_DECREF(slotstate_copy);
                slotstate_copy = NULL;
            }
        }
    }

    // Handle listitems
    if (listitems) {
        append = PyObject_GetAttr(inst, module_state.str_append);
        if (!append)
            goto error;

        it = PyObject_GetIter(listitems);
        if (!it) {
            Py_DECREF(append);
            append = NULL;
            goto error;
        }

        PyObject* loop_item;
        while ((loop_item = PyIter_Next(it)) != NULL) {
            item_copy = deepcopy(loop_item, memo);
            Py_DECREF(loop_item);
            if (!item_copy) {
                Py_DECREF(it);
                it = NULL;
                Py_DECREF(append);
                append = NULL;
                goto error;
            }

            result = PyObject_CallOneArg(append, item_copy);
            Py_DECREF(item_copy);
            item_copy = NULL;
            if (!result) {
                Py_DECREF(it);
                it = NULL;
                Py_DECREF(append);
                append = NULL;
                goto error;
            }
            Py_DECREF(result);
            result = NULL;
        }

        if (PyErr_Occurred()) {
            Py_DECREF(it);
            it = NULL;
            Py_DECREF(append);
            append = NULL;
            goto error;
        }

        Py_DECREF(it);
        it = NULL;
        Py_DECREF(append);
        append = NULL;
    }

    // Handle dictitems
    if (dictitems) {
        it = PyObject_GetIter(dictitems);
        if (!it)
            goto error;

        PyObject* loop_pair;
        while ((loop_pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(loop_pair) || PyTuple_GET_SIZE(loop_pair) != 2) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
                goto error;
            }

            PyObject* key = PyTuple_GET_ITEM(loop_pair, 0);
            PyObject* value = PyTuple_GET_ITEM(loop_pair, 1);

            key_copy = deepcopy(key, memo);
            if (!key_copy) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                goto error;
            }

            value_copy = deepcopy(value, memo);
            if (!value_copy) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                goto error;
            }

            Py_DECREF(loop_pair);

            int status = PyObject_SetItem(inst, key_copy, value_copy);
            Py_DECREF(key_copy);
            key_copy = NULL;
            Py_DECREF(value_copy);
            value_copy = NULL;

            if (status < 0) {
                Py_DECREF(it);
                it = NULL;
                goto error;
            }
        }

        if (PyErr_Occurred()) {
            Py_DECREF(it);
            it = NULL;
            goto error;
        }

        Py_DECREF(it);
        it = NULL;
    }

    // Keep alive original object if reconstruction returned different object
    if (inst != obj) {
        if (keepalive_append(&memo->keepalive, obj) < 0)
            goto error;
    }

    Py_DECREF(reduce_result);
    return inst;

error:
    Py_XDECREF(inst);
    Py_XDECREF(reduce_result);
    Py_XDECREF(setstate);
    Py_XDECREF(state_copy);
    Py_XDECREF(result);
    Py_XDECREF(dict_state_copy);
    Py_XDECREF(inst_dict);
    Py_XDECREF(slotstate_copy);
    Py_XDECREF(append);
    Py_XDECREF(it);
    Py_XDECREF(item_copy);
    Py_XDECREF(key_copy);
    Py_XDECREF(value_copy);
    return NULL;
}
#endif  // _COPIUM_DEEPCOPY_C