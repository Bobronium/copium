#ifndef _COPIUM_COPY_API_C
#define _COPIUM_COPY_API_C

#include "_deepcopy.c"
#include "_deepcopy_legacy.c"
#include "_copy.c"
#include "_type_checks.c"

PyObject* py_copy(PyObject* self, PyObject* obj) {
    (void)self;

    {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            return Py_NewRef(obj);
        }
    }

    if (PySlice_Check(obj))
        return Py_NewRef(obj);
    if (PyFrozenSet_CheckExact(obj))
        return Py_NewRef(obj);

    if (PyType_IsSubtype(Py_TYPE(obj), &PyType_Type))
        return Py_NewRef(obj);

    if (is_empty_initializable(obj)) {
        PyObject* fresh = make_empty_same_type(obj);
        if (fresh == Py_None)
            Py_DECREF(fresh);
        else
            return fresh;
    }

    {
        PyObject* maybe = try_stdlib_mutable_copy(obj);
        if (!maybe)
            return NULL;
        if (maybe != Py_None)
            return maybe;
        Py_DECREF(maybe);
    }

    {
        PyObject* cp = PyObject_GetAttrString(obj, "__copy__");
        if (cp) {
            PyObject* out = PyObject_CallNoArgs(cp);
            Py_DECREF(cp);
            return out;
        }
        PyErr_Clear();
    }

    PyTypeObject* obj_type = Py_TYPE(obj);
    PyObject* reduce_result = try_reduce_via_registry(obj, obj_type);
    if (!reduce_result) {
        if (PyErr_Occurred())
            return NULL;
        reduce_result = call_reduce_method_preferring_ex(obj);
        if (!reduce_result)
            return NULL;
    }

    PyObject *constructor = NULL, *args = NULL, *state = NULL, *listiter = NULL, *dictiter = NULL;
    int unpack_result =
        validate_reduce_tuple(reduce_result, &constructor, &args, &state, &listiter, &dictiter);
    if (unpack_result == REDUCE_ERROR) {
        Py_DECREF(reduce_result);
        return NULL;
    }
    if (unpack_result == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(obj);
    }

    PyObject* out = NULL;
    if (PyTuple_GET_SIZE(args) == 0) {
        out = PyObject_CallNoArgs(constructor);
    } else {
        out = PyObject_CallObject(constructor, args);
    }
    if (!out) {
        Py_DECREF(reduce_result);
        return NULL;
    }

    if ((state && state != Py_None) || (listiter && listiter != Py_None) ||
        (dictiter && dictiter != Py_None)) {
        PyObject* applied = reconstruct_state(
            out,
            state ? state : Py_None,
            listiter ? listiter : Py_None,
            dictiter ? dictiter : Py_None
        );
        if (!applied) {
            Py_DECREF(out);
            Py_DECREF(reduce_result);
            return NULL;
        }
        Py_DECREF(applied);
    }

    Py_DECREF(reduce_result);
    return out;
}

PyObject* py_deepcopy(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    PyObject* obj = NULL;
    PyObject* memo_arg = Py_None;

    if (!kwnames || PyTuple_GET_SIZE(kwnames) == 0) {
        if (UNLIKELY(nargs < 1)) {
            PyErr_Format(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
            return NULL;
        }
        if (UNLIKELY(nargs > 2)) {
            PyErr_Format(
                PyExc_TypeError,
                "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                nargs
            );
            return NULL;
        }
        obj = args[0];
        memo_arg = (nargs == 2) ? args[1] : Py_None;
        goto have_args;
    }

    const Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    if (kwcount == 1) {
        PyObject* kw0 = PyTuple_GET_ITEM(kwnames, 0);
        const int is_memo =
            PyUnicode_Check(kw0) && PyUnicode_CompareWithASCIIString(kw0, "memo") == 0;

        if (is_memo) {
            if (UNLIKELY(nargs < 1)) {
                PyErr_Format(
                    PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'"
                );
                return NULL;
            }
            if (UNLIKELY(nargs > 2)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                    nargs
                );
                return NULL;
            }
            if (UNLIKELY(nargs == 2)) {
                PyErr_SetString(
                    PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'"
                );
                return NULL;
            }
            obj = args[0];
            memo_arg = args[nargs + 0];
            goto have_args;
        }
    }

    {
        Py_ssize_t i;
        int seen_memo_kw = 0;

        if (UNLIKELY(nargs > 2)) {
            PyErr_Format(
                PyExc_TypeError,
                "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                nargs
            );
            return NULL;
        }

        if (nargs >= 1)
            obj = args[0];
        if (nargs == 2)
            memo_arg = args[1];

        const Py_ssize_t kwc = PyTuple_GET_SIZE(kwnames);
        for (i = 0; i < kwc; i++) {
            PyObject* name = PyTuple_GET_ITEM(kwnames, i);
            PyObject* val = args[nargs + i];

            if (!(PyUnicode_Check(name))) {
                PyErr_SetString(PyExc_TypeError, "deepcopy() keywords must be strings");
                return NULL;
            }

            if (PyUnicode_CompareWithASCIIString(name, "x") == 0) {
                if (UNLIKELY(obj != NULL)) {
                    PyErr_SetString(
                        PyExc_TypeError, "deepcopy() got multiple values for argument 'x'"
                    );
                    return NULL;
                }
                obj = val;
                continue;
            }

            if (PyUnicode_CompareWithASCIIString(name, "memo") == 0) {
                if (UNLIKELY(seen_memo_kw || nargs == 2)) {
                    PyErr_SetString(
                        PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'"
                    );
                    return NULL;
                }
                memo_arg = val;
                seen_memo_kw = 1;
                continue;
            }

            PyErr_Format(
                PyExc_TypeError, "deepcopy() got an unexpected keyword argument '%U'", name
            );
            return NULL;
        }

        if (UNLIKELY(obj == NULL)) {
            PyErr_Format(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
            return NULL;
        }
    }

have_args:
    if (memo_arg == Py_None) {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            return Py_NewRef(obj);
        }
        PyObject* memo_local = get_tss_memo();
        if (!memo_local)
            return NULL;
        MemoObject* memo = (MemoObject*)memo_local;

        PyObject* result = deepcopy(obj, memo);
        cleanup_tss_memo(memo, memo_local);
        return result;
    }

    PyObject* result = NULL;

    if (Py_TYPE(memo_arg) == &Memo_Type) {
        MemoObject* memo = (MemoObject*)memo_arg;
        Py_INCREF(memo_arg);
        result = deepcopy(obj, memo);
        Py_DECREF(memo_arg);
        return result;
    }

    else {
        /* deepcopy_py handles version-specific ordering of immutable check vs memo lookup */
        Py_INCREF(memo_arg);
        PyObject* memo = memo_arg;
        PyObject* keep_list = NULL; /* lazily created on first append */

        result = deepcopy_legacy(obj, memo, &keep_list);

        Py_XDECREF(keep_list);
        Py_DECREF(memo);
        return result;
    }
}

#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
PyObject* py_replace(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;
    if (UNLIKELY(nargs == 0)) {
        PyErr_SetString(PyExc_TypeError, "replace() missing 1 required positional argument: 'obj'");
        return NULL;
    }
    if (UNLIKELY(nargs > 1)) {
        PyErr_Format(
            PyExc_TypeError, "replace() takes 1 positional argument but %zd were given", nargs
        );
        return NULL;
    }
    PyObject* obj = args[0];
    PyObject* cls = (PyObject*)Py_TYPE(obj);

    PyObject* func = PyObject_GetAttrString(cls, "__replace__");
    if (!func) {
        PyErr_Clear();
        PyErr_Format(
            PyExc_TypeError, "replace() does not support %.200s objects", Py_TYPE(obj)->tp_name
        );
        return NULL;
    }
    if (!PyCallable_Check(func)) {
        Py_DECREF(func);
        PyErr_SetString(PyExc_TypeError, "__replace__ is not callable");
        return NULL;
    }

    PyObject* posargs = PyTuple_New(1);
    if (!posargs) {
        Py_DECREF(func);
        return NULL;
    }
    Py_INCREF(obj);
    PyTuple_SET_ITEM(posargs, 0, obj);

    PyObject* kwargs = NULL;
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        kwargs = PyDict_New();
        if (!kwargs) {
            Py_DECREF(func);
            Py_DECREF(posargs);
            return NULL;
        }
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            PyObject* key = PyTuple_GET_ITEM(kwnames, i);
            PyObject* val = args[nargs + i];
            if (PyDict_SetItem(kwargs, key, val) < 0) {
                Py_DECREF(func);
                Py_DECREF(posargs);
                Py_DECREF(kwargs);
                return NULL;
            }
        }
    }

    PyObject* out = PyObject_Call(func, posargs, kwargs);
    Py_DECREF(func);
    Py_DECREF(posargs);
    Py_XDECREF(kwargs);
    return out;
}
#endif

#endif