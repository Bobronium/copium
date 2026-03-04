/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium.extra submodule
 *
 * Batch copying utilities:
 *   - replicate(obj, n) - create n deep copies
 *   - repeatcall(fn, n) - call fn() n times, collect results
 */
#ifndef COPIUM_EXTRA_C
#define COPIUM_EXTRA_C

#include "_common.h"
#include "_state.c"
#include "_type_checks.c"
#include "_memo.c"
#include "_deepcopy.c"
#include "_extra.c"

PyObject* py_replicate(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;

    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "replicate(obj, n, /)");
        return NULL;
    }

    PyObject* obj = args[0];

    long n_long = PyLong_AsLong(args[1]);
    if (PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 0");
        return NULL;
    }
    Py_ssize_t n = (Py_ssize_t)n_long;

    if (kwnames) {
        PyErr_SetString(PyExc_TypeError, "replicate() does not accept keyword arguments");
        return NULL;
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
        PyObject* out = PyList_New(n);
        if (!out)
            return NULL;

        int is_tss;
        PyMemoObject* memo = get_memo(&is_tss);
        if (!memo) {
            Py_DECREF(out);
            return NULL;
        }

        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* copy_i = deepcopy(obj, memo);
            if (!cleanup_memo(memo, is_tss)) {
                memo = get_memo(&is_tss);
                if (!memo) {
                    Py_DECREF(out);
                    return NULL;
                }
            }

            if (!copy_i) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, copy_i);
        }
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

/* ------------------------------------------------------------------------- */

static PyMethodDef extra_methods[] = {
    {"replicate",
     (PyCFunction)(void*)py_replicate,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "replicate(obj, n, /)\n--\n\n"
         "Returns n deep copies of the object in a list.\n\n"
         "Equivalent of [deepcopy(obj) for _ in range(n)], but faster."
     )},
    {"repeatcall",
     (PyCFunction)(void*)py_repeatcall,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "repeatcall(function, size, /)\n--\n\n"
         "Call function repeatedly size times and return the list of results.\n\n"
         "Equivalent of [function() for _ in range(size)], but faster."
     )},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef extra_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.extra",
    "Batch copying utilities for copium.",
    -1,
    extra_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

#endif /* COPIUM_EXTRA_C */
