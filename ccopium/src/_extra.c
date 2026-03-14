/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_EXTRA_C
#define _COPIUM_EXTRA_C
#include "_common.h"

static ALWAYS_INLINE PyObject* build_list_by_calling_noargs(PyObject* callable, Py_ssize_t n) {
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 0");
        return NULL;
    }
    PyObject* out = PyList_New(n);
    if (!out)
        return NULL;

    vectorcallfunc vc = PyVectorcall_Function(callable);
    if (LIKELY(vc)) {
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = vc(callable, NULL, PyVectorcall_NARGS(0), NULL);
            if (!item) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, item);
        }
    } else {
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = PyObject_CallNoArgs(callable);
            if (!item) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, item);
        }
    }
    return out;
}

#endif  // _COPIUM_EXTRA_C