/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Unified module initialization and cleanup.
 *
 * All subsystem initialization is orchestrated here with proper ordering
 * and cleanup on failure. Each subsystem that needs lifecycle management
 * exposes xxx_module_init() / xxx_module_cleanup() pair.
 */
#ifndef _COPIUM_INIT_C
#define _COPIUM_INIT_C

#include "_common.h"
#include "_state.c"
#include "_dict_iter.c"
#include "_memo.c"
#include "_pinning.c"

/* Tracks initialization state for proper cleanup in reverse order */
static struct {
    int strings_ready;
    int types_ready;
    int copyreg_ready;
    int sentinel_ready;
    int memo_tss_ready;
    int dict_iter_ready;
    int memo_types_ready;
    int pinning_ready;
    int error_attr_ready;
} _init_state = {0};

static void _copium_cleanup(void) {
    /* Clean up in reverse order of initialization */

    if (_init_state.dict_iter_ready) {
        dict_iter_module_cleanup();
        _init_state.dict_iter_ready = 0;
    }

    if (_init_state.memo_tss_ready) {
        if (PyThread_tss_is_created(&module_state.memo_tss)) {
            PyThread_tss_delete(&module_state.memo_tss);
        }
        _init_state.memo_tss_ready = 0;
    }

    if (_init_state.sentinel_ready) {
        Py_CLEAR(module_state.sentinel);
        _init_state.sentinel_ready = 0;
    }

    if (_init_state.copyreg_ready) {
        Py_CLEAR(module_state.copyreg_dispatch);
        Py_CLEAR(module_state.copy_Error);
        Py_CLEAR(module_state.copyreg_newobj);
        Py_CLEAR(module_state.copyreg_newobj_ex);
        Py_CLEAR(module_state.create_precompiler_reconstructor);
        _init_state.copyreg_ready = 0;
    }

    if (_init_state.types_ready) {
        Py_CLEAR(module_state.BuiltinFunctionType);
        Py_CLEAR(module_state.MethodType);
        Py_CLEAR(module_state.CodeType);
        Py_CLEAR(module_state.range_type);
        Py_CLEAR(module_state.property_type);
        Py_CLEAR(module_state.weakref_ref_type);
        Py_CLEAR(module_state.re_Pattern_type);
        Py_CLEAR(module_state.Decimal_type);
        Py_CLEAR(module_state.Fraction_type);
        _init_state.types_ready = 0;
    }

    if (_init_state.strings_ready) {
        Py_CLEAR(module_state.str_reduce_ex);
        Py_CLEAR(module_state.str_reduce);
        Py_CLEAR(module_state.str_deepcopy);
        Py_CLEAR(module_state.str_setstate);
        Py_CLEAR(module_state.str_dict);
        Py_CLEAR(module_state.str_append);
        Py_CLEAR(module_state.str_update);
        Py_CLEAR(module_state.str_new);
        Py_CLEAR(module_state.str_get);
        _init_state.strings_ready = 0;
    }
}

/* -------------------------------------------------------------------------- */

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
            return -1;                                                                 \
        }                                                                              \
        module_state.target_field = (PyTypeObject*)_loaded_type;                       \
    } while (0)

static int _init_strings(void) {
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
        return -1;
    }
    return 0;
}

static int _init_types(void) {
    PyObject* mod_types = NULL;
    PyObject* mod_builtins = NULL;
    PyObject* mod_weakref = NULL;
    PyObject* mod_re = NULL;
    PyObject* mod_decimal = NULL;
    PyObject* mod_fractions = NULL;
    int result = -1;

    mod_types = PyImport_ImportModule("types");
    if (!mod_types)
        goto done;

    mod_builtins = PyImport_ImportModule("builtins");
    if (!mod_builtins)
        goto done;

    mod_weakref = PyImport_ImportModule("weakref");
    if (!mod_weakref)
        goto done;

    mod_re = PyImport_ImportModule("re");
    if (!mod_re)
        goto done;

    mod_decimal = PyImport_ImportModule("decimal");
    if (!mod_decimal)
        goto done;

    mod_fractions = PyImport_ImportModule("fractions");
    if (!mod_fractions)
        goto done;

    LOAD_TYPE(mod_types, "BuiltinFunctionType", BuiltinFunctionType);
    LOAD_TYPE(mod_types, "CodeType", CodeType);
    LOAD_TYPE(mod_types, "MethodType", MethodType);
    LOAD_TYPE(mod_builtins, "property", property_type);
    LOAD_TYPE(mod_builtins, "range", range_type);
    LOAD_TYPE(mod_weakref, "ref", weakref_ref_type);
    LOAD_TYPE(mod_re, "Pattern", re_Pattern_type);
    LOAD_TYPE(mod_decimal, "Decimal", Decimal_type);
    LOAD_TYPE(mod_fractions, "Fraction", Fraction_type);

    result = 0;

done:
    Py_XDECREF(mod_types);
    Py_XDECREF(mod_builtins);
    Py_XDECREF(mod_weakref);
    Py_XDECREF(mod_re);
    Py_XDECREF(mod_decimal);
    Py_XDECREF(mod_fractions);

    if (result < 0 && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to import required stdlib modules");
    }
    return result;
}

static int _init_copy_and_copyreg(void) {
    PyObject* mod_copyreg = NULL;
    PyObject* mod_copy = NULL;
    int result = -1;

    mod_copyreg = PyImport_ImportModule("copyreg");
    if (!mod_copyreg)
        goto done;

    module_state.copyreg_dispatch = PyObject_GetAttrString(mod_copyreg, "dispatch_table");
    if (!module_state.copyreg_dispatch || !PyDict_Check(module_state.copyreg_dispatch)) {
        PyErr_SetString(PyExc_ImportError, "copium: copyreg.dispatch_table missing or not a dict");
        goto done;
    }

    module_state.copyreg_newobj = PyObject_GetAttrString(mod_copyreg, "__newobj__");
    if (!module_state.copyreg_newobj) {
        PyErr_Clear();
        module_state.copyreg_newobj = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg_newobj)
            goto done;
    }

    module_state.copyreg_newobj_ex = PyObject_GetAttrString(mod_copyreg, "__newobj_ex__");
    if (!module_state.copyreg_newobj_ex) {
        PyErr_Clear();
        module_state.copyreg_newobj_ex = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg_newobj_ex)
            goto done;
    }

    mod_copy = PyImport_ImportModule("copy");
    if (!mod_copy) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to import copy module");
        goto done;
    }

    module_state.copy_Error = PyObject_GetAttrString(mod_copy, "Error");
    if (!module_state.copy_Error || !PyExceptionClass_Check(module_state.copy_Error)) {
        PyErr_SetString(PyExc_ImportError, "copium: copy.Error missing or not an exception");
        goto done;
    }

    result = 0;

done:
    Py_XDECREF(mod_copyreg);
    Py_XDECREF(mod_copy);
    return result;
}

static int _init_pinning(PyObject* module) {
    PyObject* mod_snapshots = PyImport_ImportModule("duper.snapshots");
    if (!mod_snapshots) {
        PyErr_Clear();
        module_state.create_precompiler_reconstructor = NULL;
        return 0; /* Not an error - pinning is optional */
    }

    module_state.create_precompiler_reconstructor = PyObject_GetAttrString(
        mod_snapshots, "create_precompiler_reconstructor"
    );
    if (!module_state.create_precompiler_reconstructor) {
        PyErr_Clear();
    }

    if (_duper_pinning_add_types(module) < 0) {
        Py_DECREF(mod_snapshots);
        return -1;
    }

    Py_DECREF(mod_snapshots);
    return 0;
}

/* -------------------------------------------------------------------------- */

int _copium_init(PyObject* module) {
    if (_init_strings() < 0)
        goto error;
    _init_state.strings_ready = 1;

    if (_init_types() < 0)
        goto error;
    _init_state.types_ready = 1;

    if (_init_copy_and_copyreg() < 0)
        goto error;
    _init_state.copyreg_ready = 1;

    module_state.sentinel = PyList_New(0);
    if (!module_state.sentinel) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create sentinel list");
        goto error;
    }
    _init_state.sentinel_ready = 1;

    if (PyThread_tss_create(&module_state.memo_tss) != 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create memo TSS");
        goto error;
    }
    _init_state.memo_tss_ready = 1;

    if (dict_iter_module_init() < 0)
        goto error;
    _init_state.dict_iter_ready = 1;

    if (memo_ready_types() < 0)
        goto error;
    _init_state.memo_types_ready = 1;

    /* Register Memo with collections.abc.MutableMapping */
    if (memo_register_abcs() < 0)
        goto error;

    if (_init_pinning(module) < 0)
        goto error;
    _init_state.pinning_ready = 1;

    if (PyObject_SetAttrString(module, "Error", module_state.copy_Error) < 0)
        goto error;
    _init_state.error_attr_ready = 1;

    return 0;

error:
    _copium_cleanup();
    return -1;
}

int _copium_duper_available(void) {
    return module_state.create_precompiler_reconstructor != NULL;
}

#endif /* _COPIUM_INIT_C */