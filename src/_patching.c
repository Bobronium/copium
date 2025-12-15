/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_PATCHING_C
#define _COPIUM_PATCHING_C

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject* py_deepcopy(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames);

#if PY_VERSION_HEX >= 0x030C0000

#include "cpython/funcobject.h"

static PyObject* copium_deepcopy_vectorcall(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames
) {
    (void)callable;
    return py_deepcopy(NULL, args, PyVectorcall_NARGS(nargsf), kwnames);
}

static int _patch_apply(PyFunctionObject* fn, PyObject* target) {
    if (PyVectorcall_Function((PyObject*)fn) == copium_deepcopy_vectorcall)
        return 0;

    vectorcallfunc original_vc = PyVectorcall_Function((PyObject*)fn);
    if (!original_vc) {
        PyErr_SetString(PyExc_RuntimeError, "copium.patch: function has no vectorcall");
        return -1;
    }

    PyObject* capsule = PyCapsule_New((void*)original_vc, "copium._original_vectorcall", NULL);
    if (!capsule)
        return -1;

    if (PyObject_SetAttrString((PyObject*)fn, "__copium_original__", capsule) < 0) {
        Py_DECREF(capsule);
        return -1;
    }
    Py_DECREF(capsule);

    if (PyObject_SetAttrString((PyObject*)fn, "__wrapped__", target) < 0) {
        PyObject_DelAttrString((PyObject*)fn, "__copium_original__");
        PyErr_Clear();
        return -1;
    }

    PyFunction_SetVectorcall(fn, copium_deepcopy_vectorcall);
    return 1;
}

static int _patch_unapply(PyFunctionObject* fn) {
    PyObject* capsule = PyObject_GetAttrString((PyObject*)fn, "__copium_original__");
    if (!capsule) {
        PyErr_Clear();
        PyErr_SetString(PyExc_RuntimeError, "copium.patch: function not patched");
        return -1;
    }

    vectorcallfunc original_vc = (vectorcallfunc)PyCapsule_GetPointer(capsule, "copium._original_vectorcall");
    Py_DECREF(capsule);

    if (!original_vc)
        return -1;

    PyFunction_SetVectorcall(fn, original_vc);

    PyObject_DelAttrString((PyObject*)fn, "__copium_original__");
    PyErr_Clear();
    PyObject_DelAttrString((PyObject*)fn, "__wrapped__");
    PyErr_Clear();

    return 0;
}

static int _patch_is_applied(PyFunctionObject* fn) {
    return PyVectorcall_Function((PyObject*)fn) == copium_deepcopy_vectorcall;
}

#else

static PyObject* g_template_code = NULL;

static int _init_template(void) {
    if (g_template_code)
        return 0;

    static const char* src =
        "def deepcopy(x, memo=None, _nil=[]):\n"
        "    return \"copium.deepcopy\"(x, memo)\n";

    PyObject* globals = PyDict_New();
    if (!globals)
        return -1;

    PyObject* builtins = PyEval_GetBuiltins();
    if (builtins && PyDict_SetItemString(globals, "__builtins__", builtins) < 0) {
        Py_DECREF(globals);
        return -1;
    }

    PyObject* warnings = PyImport_ImportModule("warnings");
    PyObject* old_filters = NULL;
    PyObject* filters_copy = NULL;
    if (warnings) {
        old_filters = PyObject_GetAttrString(warnings, "filters");
        if (old_filters)
            filters_copy = PySequence_List(old_filters);
        PyObject* ignore = PyObject_CallMethod(
            warnings, "simplefilter", "sO", "ignore", PyExc_SyntaxWarning
        );
        Py_XDECREF(ignore);
    }

    PyObject* res = PyRun_StringFlags(src, Py_file_input, globals, globals, NULL);

    if (warnings && filters_copy)
        PyObject_SetAttrString(warnings, "filters", filters_copy);
    Py_XDECREF(filters_copy);
    Py_XDECREF(old_filters);
    Py_XDECREF(warnings);

    if (!res) {
        Py_DECREF(globals);
        return -1;
    }
    Py_DECREF(res);

    PyObject* fn = PyDict_GetItemString(globals, "deepcopy");
    if (!fn) {
        Py_DECREF(globals);
        PyErr_SetString(PyExc_RuntimeError, "copium.patch: template creation failed");
        return -1;
    }

    g_template_code = PyObject_GetAttrString(fn, "__code__");
    Py_DECREF(globals);
    return g_template_code ? 0 : -1;
}

static PyObject* _build_patched_code(PyObject* target) {
    PyObject* template_consts = PyObject_GetAttrString(g_template_code, "co_consts");
    if (!template_consts)
        return NULL;

    Py_ssize_t n = PyTuple_GET_SIZE(template_consts);
    Py_ssize_t sentinel_idx = -1;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = PyTuple_GET_ITEM(template_consts, i);
        if (PyUnicode_Check(item) &&
            PyUnicode_CompareWithASCIIString(item, "copium.deepcopy") == 0) {
            sentinel_idx = i;
            break;
        }
    }

    if (sentinel_idx < 0) {
        Py_DECREF(template_consts);
        PyErr_SetString(PyExc_RuntimeError, "copium.patch: sentinel not found");
        return NULL;
    }

    PyObject* new_consts = PyList_New(n);
    if (!new_consts) {
        Py_DECREF(template_consts);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = (i == sentinel_idx) ? target : PyTuple_GET_ITEM(template_consts, i);
        Py_INCREF(item);
        PyList_SET_ITEM(new_consts, i, item);
    }
    Py_DECREF(template_consts);

    PyObject* consts_tuple = PyList_AsTuple(new_consts);
    Py_DECREF(new_consts);
    if (!consts_tuple)
        return NULL;

    PyObject* replace = PyObject_GetAttrString(g_template_code, "replace");
    if (!replace) {
        Py_DECREF(consts_tuple);
        return NULL;
    }

    PyObject* kwargs = PyDict_New();
    if (!kwargs) {
        Py_DECREF(replace);
        Py_DECREF(consts_tuple);
        return NULL;
    }
    PyDict_SetItemString(kwargs, "co_consts", consts_tuple);
    Py_DECREF(consts_tuple);

    PyObject* empty = PyTuple_New(0);
    PyObject* new_code = PyObject_Call(replace, empty, kwargs);
    Py_DECREF(empty);
    Py_DECREF(replace);
    Py_DECREF(kwargs);

    return new_code;
}

static void _cleanup_patch_attrs(PyFunctionObject* fn) {
    PyObject_DelAttrString((PyObject*)fn, "__copium_original__");
    PyErr_Clear();
    PyObject_DelAttrString((PyObject*)fn, "__wrapped__");
    PyErr_Clear();
}

static int _patch_apply(PyFunctionObject* fn, PyObject* target) {
    if (_init_template() < 0)
        return -1;

    if (PyObject_HasAttrString((PyObject*)fn, "__copium_original__"))
        return 0;

    PyObject* current_code = PyObject_GetAttrString((PyObject*)fn, "__code__");
    if (!current_code)
        return -1;

    if (PyObject_SetAttrString((PyObject*)fn, "__copium_original__", current_code) < 0) {
        Py_DECREF(current_code);
        return -1;
    }
    Py_DECREF(current_code);

    if (PyObject_SetAttrString((PyObject*)fn, "__wrapped__", target) < 0) {
        _cleanup_patch_attrs(fn);
        return -1;
    }

    PyObject* new_code = _build_patched_code(target);
    if (!new_code) {
        _cleanup_patch_attrs(fn);
        return -1;
    }

    if (PyObject_SetAttrString((PyObject*)fn, "__code__", new_code) < 0) {
        Py_DECREF(new_code);
        _cleanup_patch_attrs(fn);
        return -1;
    }
    Py_DECREF(new_code);

    return 1;
}

static int _patch_unapply(PyFunctionObject* fn) {
    PyObject* original_code = PyObject_GetAttrString((PyObject*)fn, "__copium_original__");
    if (!original_code) {
        PyErr_Clear();
        PyErr_SetString(PyExc_RuntimeError, "copium.patch: not applied");
        return -1;
    }

    if (PyObject_SetAttrString((PyObject*)fn, "__code__", original_code) < 0) {
        Py_DECREF(original_code);
        return -1;
    }
    Py_DECREF(original_code);

    _cleanup_patch_attrs(fn);

    return 0;
}

static int _patch_is_applied(PyFunctionObject* fn) {
    return PyObject_HasAttrString((PyObject*)fn, "__copium_original__");
}

#endif

#endif
