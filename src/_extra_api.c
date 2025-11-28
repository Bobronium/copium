#ifndef _COPIUM_EXTRA_API_C
#define _COPIUM_EXTRA_API_C

#include "_deepcopy.c"
#include "_extra.c"
#include "_type_checks.c"

PyObject* py_replicate(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;

    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "replicate(obj, n, /, *, compile_after=20)");
        return NULL;
    }

    PyObject* obj = args[0];

    long n_long = PyLong_AsLong(args[1]);
    if (n_long == -1 && PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 0");
        return NULL;
    }
    Py_ssize_t n = (Py_ssize_t)n_long;

    int duper_available = (module_state.create_precompiler_reconstructor != NULL);

    int compile_after = 20;
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        if (kwcount > 1) {
            PyErr_SetString(PyExc_TypeError, "replicate accepts only 'compile_after' keyword");
            return NULL;
        }
        if (kwcount == 1) {
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, 0);
            int is_compile_after = PyUnicode_Check(kwname) &&
                (PyUnicode_CompareWithASCIIString(kwname, "compile_after") == 0);
            if (!is_compile_after) {
                PyErr_SetString(
                    PyExc_TypeError, "unknown keyword; only 'compile_after' is supported"
                );
                return NULL;
            }
            if (!duper_available) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "replicate(): 'compile_after' requires duper.snapshots; it is not available"
                );
                return NULL;
            }
            PyObject* kwval = args[nargs + 0];
            long ca = PyLong_AsLong(kwval);
            if (ca == -1 && PyErr_Occurred())
                return NULL;
            if (ca < 0) {
                PyErr_SetString(PyExc_ValueError, "compile_after must be >= 0");
                return NULL;
            }
            compile_after = (int)ca;
        }
    }

    if (n == 0)
        return PyList_New(0);

    {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            PyObject* out = PyList_New(n);
            if (!out)
                return NULL;
            for (Py_ssize_t i = 0; i < n; i++) {
                {
                    PyObject* copy_i = Py_NewRef(obj);
                    PyList_SET_ITEM(out, i, copy_i);
                }
            }
            return out;
        }
    }

    {
        PinObject* pin = _duper_lookup_pin_for_object(obj);
        if (pin) {
            PyObject* factory = pin->factory;
            if (UNLIKELY(!factory || !PyCallable_Check(factory))) {
                PyErr_SetString(PyExc_RuntimeError, "pinned object has no valid factory");
                return NULL;
            }
            PyObject* out = build_list_by_calling_noargs(factory, n);
            if (out)
                pin->hits += (uint64_t)n;
            return out;
        }
    }

    if (!duper_available || n <= (Py_ssize_t)compile_after) {
        PyObject* out = PyList_New(n);
        if (!out)
            return NULL;

        PyObject* memo_local = get_tss_memo();
        if (!memo_local)
            return NULL;
        MemoObject* memo = (MemoObject*)memo_local;

        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* copy_i = deepcopy(obj, memo);

            if (!cleanup_tss_memo(memo, memo_local)) {
                PyObject* memo_local = get_tss_memo();
                if (!memo_local) {
                    Py_DECREF(out);
                    return NULL;
                }
                memo = (MemoObject*)memo_local;
            }

            if (!copy_i) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, copy_i);
        }
        return out;
    }

    {
        PyObject* cpr = module_state.create_precompiler_reconstructor;
        if (UNLIKELY(!cpr || !PyCallable_Check(cpr))) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "duper.snapshots.create_precompiler_reconstructor is not callable"
            );
            return NULL;
        }

        PyObject* reconstructor = PyObject_CallOneArg(cpr, obj);
        if (!reconstructor)
            return NULL;

        if (UNLIKELY(!PyCallable_Check(reconstructor))) {
            Py_DECREF(reconstructor);
            PyErr_SetString(PyExc_TypeError, "reconstructor must be callable (FunctionType)");
            return NULL;
        }

        PyObject* out = build_list_by_calling_noargs(reconstructor, n);
        Py_DECREF(reconstructor);
        return out;
    }
}

PyObject* py_repeatcall(
    PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
) {
    (void)self;
    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "repeatcall(function, size, /)");
        return NULL;
    }
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        PyErr_SetString(PyExc_TypeError, "repeatcall() takes no keyword arguments");
        return NULL;
    }

    PyObject* func = args[0];
    if (UNLIKELY(!PyCallable_Check(func))) {
        PyErr_SetString(PyExc_TypeError, "function must be callable");
        return NULL;
    }

    long n_long = PyLong_AsLong(args[1]);
    if (n_long == -1 && PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 0");
        return NULL;
    }

    Py_ssize_t n = (Py_ssize_t)n_long;
    return build_list_by_calling_noargs(func, n);
}

#endif