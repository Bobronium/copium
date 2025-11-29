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
 *
 * Low-level patching API (apply/unapply/applied/get_vectorcall_ptr)
 * is added via _copium_patching_add_api() from _patching.c
 */
#ifndef COPIUM_PATCH_C
#define COPIUM_PATCH_C

#include "copium_common.h"

/* ------------------------------------------------------------------------- */

static PyObject* _get_attr_str(PyObject* obj, const char* name) {
    PyObject* attr = PyObject_GetAttrString(obj, name);
    if (!attr) {
        PyErr_Format(PyExc_AttributeError, "object has no attribute '%s'", name);
        return NULL;
    }
    return attr;
}

static int _truthy(PyObject* obj) {
    if (obj == NULL)
        return -1;
    int result = PyObject_IsTrue(obj);
    Py_DECREF(obj);
    return result;
}

/* ------------------------------------------------------------------------- */

static PyObject* py_enable(PyObject* self, PyObject* noargs) {
    (void)noargs;
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* py_deepcopy_object = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!py_deepcopy_object)
        return NULL;

    if (!PyFunction_Check(py_deepcopy_object)) {
        Py_DECREF(py_deepcopy_object);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    /* Get applied() from this module (copium.patch) */
    PyObject* function_applied = _get_attr_str(self, "applied");
    if (!function_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* is_applied = PyObject_CallOneArg(function_applied, py_deepcopy_object);
    Py_DECREF(function_applied);
    if (!is_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    int already = _truthy(is_applied);
    if (already < 0) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }
    if (already) {
        Py_DECREF(py_deepcopy_object);
        Py_RETURN_FALSE;
    }

    /* Get native deepcopy from main copium module */
    PyObject* copium_main = PyImport_ImportModule("copium");
    if (!copium_main) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }
    PyObject* native_deepcopy = _get_attr_str(copium_main, "deepcopy");
    Py_DECREF(copium_main);
    if (!native_deepcopy) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    /* Get apply() from this module */
    PyObject* function_apply = _get_attr_str(self, "apply");
    if (!function_apply) {
        Py_DECREF(py_deepcopy_object);
        Py_DECREF(native_deepcopy);
        return NULL;
    }

    PyObject* result =
        PyObject_CallFunction(function_apply, "OO", py_deepcopy_object, native_deepcopy);
    Py_DECREF(function_apply);
    Py_DECREF(py_deepcopy_object);
    Py_DECREF(native_deepcopy);
    if (!result)
        return NULL;
    Py_DECREF(result);

    Py_RETURN_TRUE;
}

static PyObject* py_disable(PyObject* self, PyObject* noargs) {
    (void)noargs;
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* py_deepcopy_object = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!py_deepcopy_object)
        return NULL;

    if (!PyFunction_Check(py_deepcopy_object)) {
        Py_DECREF(py_deepcopy_object);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    /* Get applied() from this module */
    PyObject* function_applied = _get_attr_str(self, "applied");
    if (!function_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* is_applied = PyObject_CallOneArg(function_applied, py_deepcopy_object);
    Py_DECREF(function_applied);
    if (!is_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    int active = _truthy(is_applied);
    if (active < 0) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }
    if (!active) {
        Py_DECREF(py_deepcopy_object);
        Py_RETURN_FALSE;
    }

    /* Get unapply() from this module */
    PyObject* function_unapply = _get_attr_str(self, "unapply");
    if (!function_unapply) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* result = PyObject_CallFunction(function_unapply, "O", py_deepcopy_object);
    Py_DECREF(function_unapply);
    Py_DECREF(py_deepcopy_object);
    if (!result)
        return NULL;
    Py_DECREF(result);

    Py_RETURN_TRUE;
}

static PyObject* py_enabled(PyObject* self, PyObject* noargs) {
    (void)noargs;
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* py_deepcopy_object = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!py_deepcopy_object)
        return NULL;

    if (!PyFunction_Check(py_deepcopy_object)) {
        Py_DECREF(py_deepcopy_object);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    /* Get applied() from this module */
    PyObject* function_applied = _get_attr_str(self, "applied");
    if (!function_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* is_applied = PyObject_CallOneArg(function_applied, py_deepcopy_object);
    Py_DECREF(function_applied);
    Py_DECREF(py_deepcopy_object);
    if (!is_applied)
        return NULL;

    int active = _truthy(is_applied);
    if (active < 0)
        return NULL;
    if (active)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}


/* ------------------------------------------------------------------------- */

static PyMethodDef patch_methods[] = {
    {"enable",
     (PyCFunction)py_enable,
     METH_NOARGS,
     PyDoc_STR(
         "enable()\n--\n\n"
         "Patch copy.deepcopy to forward to copium.deepcopy.\n\n"
         ":return: True if copium was enabled, False if it was already enabled."
     )},
    {"disable",
     (PyCFunction)py_disable,
     METH_NOARGS,
     PyDoc_STR(
         "disable()\n--\n\n"
         "Undo enable(): restore original copy.deepcopy if applied.\n\n"
         ":return: True if copium was disabled, False if it was already disabled."
     )},
    {"enabled",
     (PyCFunction)py_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "enabled()\n--\n\n"
         "Return True if copy.deepcopy is currently patched to use copium.\n\n"
         ":return: Whether copium is currently enabled."
     )},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef patch_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.patch",
    "Monkey-patching utilities for stdlib copy module.",
    -1,
    patch_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

#endif /* COPIUM_PATCH_C */