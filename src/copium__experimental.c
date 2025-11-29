/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium._experimental submodule
 *
 * Experimental Pin API (requires duper.snapshots):
 *   - pin(obj)           - create a pin for obj
 *   - unpin(obj)         - remove pin
 *   - pinned(obj)        - get pin or None
 *   - clear_pins()       - remove all pins
 *   - get_pins()         - get live mapping of pins
 *
 * This submodule is only created when duper.snapshots is available.
 */
#ifndef COPIUM__EXPERIMENTAL_C
#define COPIUM__EXPERIMENTAL_C

#include "copium_common.h"
#include "_pinning.c"


PyObject* py_pin(PyObject* self, PyObject* obj) {
    (void)self;
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "pin(obj) missing obj");
        return NULL;
    }
    PinObject* pin = create_pin_for_object(obj);
    if (!pin)
        return NULL;
    if (pin_table_insert(&global_pin_table, (void*)obj, pin) < 0) {
        Py_DECREF(pin);
        PyErr_SetString(PyExc_RuntimeError, "pin: failed to store Pin");
        return NULL;
    }
    return (PyObject*)pin;
}

PyObject* py_unpin(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;
    if (UNLIKELY(nargs < 1)) {
        PyErr_SetString(PyExc_TypeError, "unpin() missing 1 required positional argument: 'obj'");
        return NULL;
    }
    PyObject* obj = args[0];
    int strict_mode = 0;

    if (kwnames) {
        Py_ssize_t keyword_count = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < keyword_count; i++) {
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, i);
            if (!PyUnicode_Check(kwname)) {
                PyErr_SetString(PyExc_TypeError, "keyword name must be str");
                return NULL;
            }
            int is_strict = PyUnicode_CompareWithASCIIString(kwname, "strict") == 0;
            if (!is_strict) {
                PyErr_SetString(PyExc_TypeError, "unpin() got an unexpected keyword argument");
                return NULL;
            }
            PyObject* kwvalue = args[nargs + i];
            int truthy = PyObject_IsTrue(kwvalue);
            if (truthy < 0)
                return NULL;
            strict_mode = truthy;
        }
    }

    if (!global_pin_table) {
        if (strict_mode) {
            PyErr_SetString(PyExc_KeyError, "object not pinned");
            return NULL;
        }
        Py_RETURN_NONE;
    }

    if (pin_table_remove(global_pin_table, (void*)obj) < 0) {
        if (strict_mode) {
            PyErr_SetString(PyExc_KeyError, "object not pinned");
            return NULL;
        }
        PyErr_Clear();
    }

    if (global_pin_table->used == 0) {
        pin_table_free(global_pin_table);
        global_pin_table = NULL;
    }
    Py_RETURN_NONE;
}

PyObject* py_pinned(PyObject* self, PyObject* obj) {
    (void)self;
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "pinned(obj) missing obj");
        return NULL;
    }
    PinObject* pin = _duper_lookup_pin_for_object(obj);
    if (!pin)
        Py_RETURN_NONE;
    Py_INCREF(pin);
    return (PyObject*)pin;
}

PyObject* py_clear_pins(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;
    if (global_pin_table) {
        pin_table_free(global_pin_table);
        global_pin_table = NULL;
    }
    Py_RETURN_NONE;
}

PyObject* py_get_pins(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;
    return PinsProxy_create_bound_to_global();
}


static PyMethodDef experimental_methods[] = {
    {"pin",
     (PyCFunction)py_pin,
     METH_O,
     PyDoc_STR("pin(obj, /)\n--\n\nReturn a Pin for obj.")},
    {"unpin",
     (PyCFunction)(void*)py_unpin,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "unpin(obj, /, *, strict=False)\n--\n\n"
         "Remove the pin for obj. If strict is True, raise if obj is not pinned."
     )},
    {"pinned",
     (PyCFunction)py_pinned,
     METH_O,
     PyDoc_STR("pinned(obj, /)\n--\n\nReturn the Pin for obj or None.")},
    {"clear_pins",
     (PyCFunction)py_clear_pins,
     METH_NOARGS,
     PyDoc_STR("clear_pins(/)\n--\n\nRemove all pins.")},
    {"get_pins",
     (PyCFunction)py_get_pins,
     METH_NOARGS,
     PyDoc_STR("get_pins(/)\n--\n\nReturn a live mapping of id(obj) -> Pin.")},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef experimental_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium._experimental",
    "Experimental Pin API (requires duper.snapshots).",
    -1,
    experimental_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

#endif /* COPIUM__EXPERIMENTAL_C */