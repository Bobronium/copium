/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_MEMO_LEGACY_C
#define _COPIUM_MEMO_LEGACY_C

#include "_state.c"

static PyObject* memo_lookup_legacy(PyObject* memo, void* key_ptr) {
    PyObject* pykey = PyLong_FromVoidPtr(key_ptr);
    if (UNLIKELY(!pykey))
        return NULL;
    PyObject* res;
    if (PyDict_CheckExact(memo)) {
        res = PyDict_GetItemWithError(memo, pykey);
        Py_DECREF(pykey);
        if (UNLIKELY(res == NULL)) {
            return NULL;
        }
        Py_INCREF(res);
        return res;
    } else {
        // exact semantics as in Python deepcopy
        res = PyObject_CallMethodObjArgs(
            memo, module_state.s__get__, pykey, module_state.sentinel, NULL
        );
        Py_DECREF(pykey);
        if (UNLIKELY(!res))
            return NULL;
        if (res == module_state.sentinel) {
            Py_DECREF(res);
            return NULL;
        }
        return res;
    }
}

static int memoize_legacy(PyObject* memo, void* key_ptr, PyObject* value) {
    PyObject* pykey = PyLong_FromVoidPtr(key_ptr);
    if (UNLIKELY(!pykey))
        return -1;
    int rc;
    if (PyDict_CheckExact(memo)) {
        rc = PyDict_SetItem(memo, pykey, value);
    } else {
        rc = PyObject_SetItem(memo, pykey, value);
    }
    Py_DECREF(pykey);
    return rc;
}

static ALWAYS_INLINE int maybe_initialize_keepalive_legacy(
    PyObject* memo, PyObject** keepalive_pointer
) {
    if (*keepalive_pointer)
        return 0;
    void* key_ptr = (void*)memo;
    PyObject* existing = memo_lookup_legacy(memo, key_ptr);
    if (existing) {
        *keepalive_pointer = existing;
        return 0;
    }
    if (PyErr_Occurred())
        return -1;
    PyObject* new_list = PyList_New(0);
    if (!new_list)
        return -1;
    if (memoize_legacy(memo, key_ptr, new_list) < 0) {
        Py_DECREF(new_list);
        return -1;
    }
    Py_DECREF(new_list);
    Py_INCREF(new_list);
    *keepalive_pointer = new_list;
    return 0;
}

// Helper for Python-dict memo keepalive: ensures keep_list exists and appends obj.
// Returns 0 on success, -1 on failure (with exception set).
static ALWAYS_INLINE int keepalive_legacy(
    PyObject* memo, PyObject** keepalive_pointer, PyObject* obj
) {
    if (*keepalive_pointer == NULL) {
        if (maybe_initialize_keepalive_legacy(memo, keepalive_pointer) < 0)
            return -1;
    }
    return PyList_Append(*keepalive_pointer, obj);
}

#endif  // _COPIUM_MEMO_LEGACY_C