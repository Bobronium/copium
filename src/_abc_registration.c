/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Shared ABC registration utilities for copium types.
 */
#ifndef _COPIUM_ABC_REGISTRATION_C
#define _COPIUM_ABC_REGISTRATION_C

#include "_common.h"

/**
 * Register a concrete type with an ABC.
 * Returns 0 on success, -1 on error.
 */
static int register_type_with_abc(PyObject* abc_type, PyObject* concrete_type) {
    PyObject* register_method = PyObject_GetAttrString(abc_type, "register");
    if (!register_method)
        return -1;
    PyObject* res = PyObject_CallOneArg(register_method, concrete_type);
    Py_DECREF(register_method);
    if (!res)
        return -1;
    Py_DECREF(res);
    return 0;
}

/**
 * Load an ABC type from collections.abc by name.
 * Returns new reference or NULL on error.
 */
static PyObject* get_collections_abc(const char* name) {
    PyObject* mod_abc = PyImport_ImportModule("collections.abc");
    if (!mod_abc)
        return NULL;
    PyObject* abc_type = PyObject_GetAttrString(mod_abc, name);
    Py_DECREF(mod_abc);
    return abc_type;
}

/**
 * Register a type with a named ABC from collections.abc.
 * Returns 0 on success, -1 on error (clears error if ABC not found).
 */
static int register_with_collections_abc(const char* abc_name, PyTypeObject* concrete_type) {
    PyObject* abc_type = get_collections_abc(abc_name);
    if (!abc_type) {
        PyErr_Clear();
        return -1;
    }
    int result = register_type_with_abc(abc_type, (PyObject*)concrete_type);
    Py_DECREF(abc_type);
    return result;
}

#endif /* _COPIUM_ABC_REGISTRATION_C */