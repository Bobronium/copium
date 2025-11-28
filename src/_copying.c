/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium
 * - Fast, native deepcopy with reduce protocol + keepalive memo
 * - Pin integration via _pinning.c (Pin/PinsProxy + APIs)
 *
 * Public API:
 *   py_deepcopy(x, memo=None) -> any
 *   py_copy(x) -> any
 *   py_replace(x, **replace=None) -> any
 *   py_replicate(x, n, /) -> any
 *
 * Python 3.10–3.14 compatible.
*/
#ifndef _COPIUM_COPYING_C
#define _COPIUM_COPYING_C

#include "copium_common.h"
#include "_state.c"
#include "_dict_iter.c"
#include "_type_checks.c"
#include "_recursion_guard.c"
#include "_reduce_helpers.c"
#include "_deepcopy.c"
#include "_deepcopy_legacy.c"
#include "_copy.c"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Python.h"


static void cleanup_on_init_failure(void) {
    Py_XDECREF(module_state.str_reduce_ex);
    Py_XDECREF(module_state.str_reduce);
    Py_XDECREF(module_state.str_deepcopy);
    Py_XDECREF(module_state.str_setstate);
    Py_XDECREF(module_state.str_dict);
    Py_XDECREF(module_state.str_append);
    Py_XDECREF(module_state.str_update);
    Py_XDECREF(module_state.str_new);
    Py_XDECREF(module_state.str_get);

    Py_XDECREF(module_state.BuiltinFunctionType);
    Py_XDECREF(module_state.MethodType);
    Py_XDECREF(module_state.CodeType);
    Py_XDECREF(module_state.range_type);
    Py_XDECREF(module_state.property_type);
    Py_XDECREF(module_state.weakref_ref_type);
    Py_XDECREF(module_state.re_Pattern_type);
    Py_XDECREF(module_state.Decimal_type);
    Py_XDECREF(module_state.Fraction_type);

    Py_XDECREF(module_state.copyreg_dispatch);
    Py_XDECREF(module_state.copy_Error);
    Py_XDECREF(module_state.copyreg_newobj);
    Py_XDECREF(module_state.copyreg_newobj_ex);
    Py_XDECREF(module_state.create_precompiler_reconstructor);
    Py_XDECREF(module_state.sentinel);

    if (PyThread_tss_is_created(&module_state.memo_tss)) {
        PyThread_tss_delete(&module_state.memo_tss);
    }
    dict_iter_module_cleanup();
}

#define LOAD_TYPE(source_module, type_name, target_field)                              \
    do {                                                                               \
        PyObject* _loaded_type = PyObject_GetAttrString((source_module), (type_name)); \
        if (!_loaded_type || !PyType_Check(_loaded_type)) {                            \
            Py_XDECREF(_loaded_type);                                                  \
            PyErr_Format(                                                              \
                PyExc_ImportError,                                                     \
                "copium: %s.%s missing or not a type",                                 \
                #source_module,                                                        \
                (type_name)                                                            \
            );                                                                         \
            goto init_error;                                                           \
        }                                                                              \
        module_state.target_field = (PyTypeObject*)_loaded_type;                       \
    } while (0)

int _copium_copying_init(PyObject* module) {
    // All module refs declared at top for cleanup
    PyObject* mod_types = NULL;
    PyObject* mod_builtins = NULL;
    PyObject* mod_weakref = NULL;
    PyObject* mod_copyreg = NULL;
    PyObject* mod_re = NULL;
    PyObject* mod_decimal = NULL;
    PyObject* mod_fractions = NULL;
    PyObject* mod_copy = NULL;

    // Intern strings
    module_state.str_reduce_ex = PyUnicode_InternFromString("__reduce_ex__");
    module_state.str_reduce = PyUnicode_InternFromString("__reduce__");
    module_state.str_deepcopy = PyUnicode_InternFromString("__deepcopy__");
    module_state.str_setstate = PyUnicode_InternFromString("__setstate__");
    module_state.str_dict = PyUnicode_InternFromString("__dict__");
    module_state.str_append = PyUnicode_InternFromString("append");
    module_state.str_update = PyUnicode_InternFromString("update");
    module_state.str_new = PyUnicode_InternFromString("__new__");
    module_state.str_get = PyUnicode_InternFromString("get");

    if (!module_state.str_reduce_ex || !module_state.str_reduce || !module_state.str_deepcopy ||
        !module_state.str_setstate || !module_state.str_dict || !module_state.str_append ||
        !module_state.str_update || !module_state.str_new || !module_state.str_get) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to intern required names");
        goto init_error;
    }

    // Load stdlib modules
    mod_types = PyImport_ImportModule("types");
    if (!mod_types)
        goto import_error;

    mod_builtins = PyImport_ImportModule("builtins");
    if (!mod_builtins)
        goto import_error;

    mod_weakref = PyImport_ImportModule("weakref");
    if (!mod_weakref)
        goto import_error;

    mod_copyreg = PyImport_ImportModule("copyreg");
    if (!mod_copyreg)
        goto import_error;

    mod_re = PyImport_ImportModule("re");
    if (!mod_re)
        goto import_error;

    mod_decimal = PyImport_ImportModule("decimal");
    if (!mod_decimal)
        goto import_error;

    mod_fractions = PyImport_ImportModule("fractions");
    if (!mod_fractions)
        goto import_error;

    // Cache types
    LOAD_TYPE(mod_types, "BuiltinFunctionType", BuiltinFunctionType);
    LOAD_TYPE(mod_types, "CodeType", CodeType);
    LOAD_TYPE(mod_types, "MethodType", MethodType);
    LOAD_TYPE(mod_builtins, "property", property_type);
    LOAD_TYPE(mod_builtins, "range", range_type);
    LOAD_TYPE(mod_weakref, "ref", weakref_ref_type);
    LOAD_TYPE(mod_re, "Pattern", re_Pattern_type);
    LOAD_TYPE(mod_decimal, "Decimal", Decimal_type);
    LOAD_TYPE(mod_fractions, "Fraction", Fraction_type);

    // copyreg dispatch and copy.Error
    module_state.copyreg_dispatch = PyObject_GetAttrString(mod_copyreg, "dispatch_table");
    if (!module_state.copyreg_dispatch || !PyDict_Check(module_state.copyreg_dispatch)) {
        PyErr_SetString(PyExc_ImportError, "copium: copyreg.dispatch_table missing or not a dict");
        goto init_error;
    }

    // Cache copyreg special constructors; if absent, use unique sentinels
    module_state.copyreg_newobj = PyObject_GetAttrString(mod_copyreg, "__newobj__");
    if (!module_state.copyreg_newobj) {
        PyErr_Clear();
        module_state.copyreg_newobj = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg_newobj)
            goto init_error;
    }
    module_state.copyreg_newobj_ex = PyObject_GetAttrString(mod_copyreg, "__newobj_ex__");
    if (!module_state.copyreg_newobj_ex) {
        PyErr_Clear();
        module_state.copyreg_newobj_ex = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg_newobj_ex)
            goto init_error;
    }

    mod_copy = PyImport_ImportModule("copy");
    if (!mod_copy) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to import copy module");
        goto init_error;
    }
    module_state.copy_Error = PyObject_GetAttrString(mod_copy, "Error");
    if (!module_state.copy_Error || !PyExceptionClass_Check(module_state.copy_Error)) {
        PyErr_SetString(PyExc_ImportError, "copium: copy.Error missing or not an exception");
        goto init_error;
    }
    Py_DECREF(mod_copy);
    mod_copy = NULL;

    // Sentinel for custom memo lookup (identity-checked empty list).
    // Safe because we create it at init time and never expose it to user code.
    module_state.sentinel = PyList_New(0);
    if (!module_state.sentinel) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create sentinel list");
        goto init_error;
    }

    // Create thread-local memo storage
    if (PyThread_tss_create(&module_state.memo_tss) != 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create memo TSS");
        goto init_error;
    }

    dict_iter_module_init();

    // Success - release module refs
    Py_DECREF(mod_types);
    Py_DECREF(mod_builtins);
    Py_DECREF(mod_weakref);
    Py_DECREF(mod_copyreg);
    Py_DECREF(mod_re);
    Py_DECREF(mod_decimal);
    Py_DECREF(mod_fractions);

    // Ready memo + keep proxy types
    if (memo_ready_types() < 0) {
        cleanup_on_init_failure();
        return -1;
    }

    // Try duper.snapshots: if available, cache reconstructor factory and expose pin API/types.
    {
        PyObject* mod_snapshots = PyImport_ImportModule("duper.snapshots");
        if (!mod_snapshots) {
            PyErr_Clear();
            module_state.create_precompiler_reconstructor = NULL;
        } else {
            module_state.create_precompiler_reconstructor =
                PyObject_GetAttrString(mod_snapshots, "create_precompiler_reconstructor");
            if (!module_state.create_precompiler_reconstructor) {
                PyErr_Clear();
            }

            if (_duper_pinning_add_types(module) < 0) {
                Py_DECREF(mod_snapshots);
                cleanup_on_init_failure();
                return -1;
            }
            Py_DECREF(mod_snapshots);
        }
    }

    if (PyObject_SetAttrString(module, "Error", module_state.copy_Error) < 0) {
        cleanup_on_init_failure();
        return -1;
    }
    return 0;

import_error:
    PyErr_SetString(PyExc_ImportError, "copium: failed to import required stdlib modules");
    // fall through
init_error:
    Py_XDECREF(mod_types);
    Py_XDECREF(mod_builtins);
    Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_copyreg);
    Py_XDECREF(mod_re);
    Py_XDECREF(mod_decimal);
    Py_XDECREF(mod_fractions);
    Py_XDECREF(mod_copy);
    cleanup_on_init_failure();
    return -1;
}

int _copium_copying_duper_available(void) {
    return module_state.create_precompiler_reconstructor != NULL;
}
#endif  // _COPIUM_COPYING_C
