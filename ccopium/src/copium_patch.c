/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium.patch submodule
 *
 * Monkey-patching utilities for stdlib copy module:
 *   - enable()  - patch copy.deepcopy to use copium
 *   - disable() - restore original copy.deepcopy
 *   - enabled() - check if patch is active
 */
#ifndef COPIUM_PATCH_C
#define COPIUM_PATCH_C

#include "_common.h"
#include "_patching.c"

static PyFunctionObject* _get_stdlib_deepcopy(void) {
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* func = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!func)
        return NULL;

    if (!PyFunction_Check(func)) {
        Py_DECREF(func);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    return (PyFunctionObject*)func;
}

static PyObject* _get_copium_deepcopy(void) {
    PyObject* copium = PyImport_ImportModule("copium");
    if (!copium)
        return NULL;

    PyObject* func = PyObject_GetAttrString(copium, "deepcopy");
    Py_DECREF(copium);
    return func;
}

static PyObject* py_enable(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;

    PyFunctionObject* stdlib_deepcopy = _get_stdlib_deepcopy();
    if (!stdlib_deepcopy)
        return NULL;

    int already = _patch_is_applied(stdlib_deepcopy);
    if (already < 0) {
        Py_DECREF(stdlib_deepcopy);
        return NULL;
    }
    if (already) {
        Py_DECREF(stdlib_deepcopy);
        Py_RETURN_FALSE;
    }

    PyObject* copium_deepcopy = _get_copium_deepcopy();
    if (!copium_deepcopy) {
        Py_DECREF(stdlib_deepcopy);
        return NULL;
    }

    int result = _patch_apply(stdlib_deepcopy, copium_deepcopy);
    Py_DECREF(stdlib_deepcopy);
    Py_DECREF(copium_deepcopy);

    if (result < 0)
        return NULL;

    Py_RETURN_TRUE;
}

static PyObject* py_disable(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;

    PyFunctionObject* stdlib_deepcopy = _get_stdlib_deepcopy();
    if (!stdlib_deepcopy)
        return NULL;

    int applied = _patch_is_applied(stdlib_deepcopy);
    if (applied < 0) {
        Py_DECREF(stdlib_deepcopy);
        return NULL;
    }
    if (!applied) {
        Py_DECREF(stdlib_deepcopy);
        Py_RETURN_FALSE;
    }

    int result = _patch_unapply(stdlib_deepcopy);
    Py_DECREF(stdlib_deepcopy);

    if (result < 0)
        return NULL;

    Py_RETURN_TRUE;
}

static PyObject* py_enabled(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;

    PyFunctionObject* stdlib_deepcopy = _get_stdlib_deepcopy();
    if (!stdlib_deepcopy)
        return NULL;

    int applied = _patch_is_applied(stdlib_deepcopy);
    Py_DECREF(stdlib_deepcopy);

    if (applied < 0)
        return NULL;

    return PyBool_FromLong(applied);
}

static PyMethodDef patch_methods[] = {
    {"enable",
     (PyCFunction)py_enable,
     METH_NOARGS,
     PyDoc_STR(
         "enable()\n--\n\n"
         "Patch copy.deepcopy to use copium. Idempotent.\n\n"
         ":return: True if state changed, False otherwise."
     )},
    {"disable",
     (PyCFunction)py_disable,
     METH_NOARGS,
     PyDoc_STR(
         "disable()\n--\n\n"
         "Restore original copy.deepcopy. Idempotent.\n\n"
         ":return: True if state changed, False otherwise."
     )},
    {"enabled",
     (PyCFunction)py_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "enabled()\n--\n\n"
         ":return: Whether copy.deepcopy is patched."
     )},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef patch_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.patch",
    "Patching utilities for stdlib copy module.",
    -1,
    patch_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

#endif