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

/* Tracks initialization state for proper cleanup in reverse order */
static struct {
    int strings_ready;
    int types_ready;
    int copyreg_ready;
    int sentinel_ready;
    int memo_tss_ready;
    int dict_iter_ready;
    int memo_types_ready;
    int error_attr_ready;
} _init_state = {0};

static void _copium_cleanup(void) {
    /* Clean up in reverse order of initialization */

    Py_CLEAR(module_state.ignored_errors);
    Py_CLEAR(module_state.ignored_errors_joined);
    module_state.memo_mode = COPIUM_MEMO_NATIVE;
    module_state.on_incompatible = COPIUM_ON_INCOMPATIBLE_WARN;

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
        Py_CLEAR(module_state.copyreg___newobj__);
        Py_CLEAR(module_state.copyreg___newobj___ex);
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
        Py_CLEAR(module_state.s__reduce_ex__);
        Py_CLEAR(module_state.s__reduce__);
        Py_CLEAR(module_state.s__deepcopy__);
        Py_CLEAR(module_state.s__setstate__);
        Py_CLEAR(module_state.s__dict__);
        Py_CLEAR(module_state.s_append);
        Py_CLEAR(module_state.s_update);
        Py_CLEAR(module_state.s__new__);
        Py_CLEAR(module_state.s__get__);
        _init_state.strings_ready = 0;
    }

    Py_CLEAR(module_state.dict_items_descr);
    module_state.dict_items_vc = NULL;
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
    module_state.s__reduce_ex__ = PyUnicode_InternFromString("__reduce_ex__");
    module_state.s__reduce__ = PyUnicode_InternFromString("__reduce__");
    module_state.s__deepcopy__ = PyUnicode_InternFromString("__deepcopy__");
    module_state.s__setstate__ = PyUnicode_InternFromString("__setstate__");
    module_state.s__dict__ = PyUnicode_InternFromString("__dict__");
    module_state.s_append = PyUnicode_InternFromString("append");
    module_state.s_update = PyUnicode_InternFromString("update");
    module_state.s__new__ = PyUnicode_InternFromString("__new__");
    module_state.s__get__ = PyUnicode_InternFromString("get");

    if (!module_state.s__reduce_ex__ || !module_state.s__reduce__ || !module_state.s__deepcopy__ ||
        !module_state.s__setstate__ || !module_state.s__dict__ || !module_state.s_append ||
        !module_state.s_update || !module_state.s__new__ || !module_state.s__get__) {
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

    module_state.copyreg___newobj__ = PyObject_GetAttrString(mod_copyreg, "__newobj__");
    if (!module_state.copyreg___newobj__) {
        PyErr_Clear();
        module_state.copyreg___newobj__ = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg___newobj__)
            goto done;
    }

    module_state.copyreg___newobj___ex = PyObject_GetAttrString(mod_copyreg, "__newobj_ex__");
    if (!module_state.copyreg___newobj___ex) {
        PyErr_Clear();
        module_state.copyreg___newobj___ex = PyObject_CallNoArgs((PyObject*)&PyBaseObject_Type);
        if (!module_state.copyreg___newobj___ex)
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

/* -------------------------------------------------------------------------- */

/* Parse COPIUM_NO_MEMO_FALLBACK_WARNING env var into a tuple of strings */
static PyObject* _parse_ignored_errors(void) {
    const char* env_val = getenv("COPIUM_NO_MEMO_FALLBACK_WARNING");
    if (!env_val || !env_val[0]) {
        return PyTuple_New(0);
    }

    /* Count separators to determine tuple size */
    Py_ssize_t count = 1;
    const char* p = env_val;
    while ((p = strstr(p, "::")) != NULL) {
        count++;
        p += 2;
    }

    PyObject* result = PyTuple_New(count);
    if (!result)
        return NULL;

    /* Split by "::" and populate tuple */
    const char* start = env_val;
    Py_ssize_t idx = 0;
    while (1) {
        const char* sep = strstr(start, "::");
        Py_ssize_t len = sep ? (sep - start) : (Py_ssize_t)strlen(start);

        if (len > 0) {
            PyObject* s = PyUnicode_FromStringAndSize(start, len);
            if (!s) {
                Py_DECREF(result);
                return NULL;
            }
            PyTuple_SET_ITEM(result, idx++, s);
        }

        if (!sep)
            break;
        start = sep + 2;
    }

    /* Shrink tuple if we skipped empty segments */
    if (idx < count) {
        if (_PyTuple_Resize(&result, idx) < 0) {
            return NULL;
        }
    }

    return result;
}

static int _copium_update_suppress_warnings(PyObject* new_tuple) {
    PyObject* old_errors = module_state.ignored_errors;
    module_state.ignored_errors = new_tuple;
    Py_XDECREF(old_errors);

    Py_CLEAR(module_state.ignored_errors_joined);
    if (PyTuple_GET_SIZE(new_tuple) > 0) {
        PyObject* sep = PyUnicode_FromString("::");
        if (!sep)
            return -1;
        module_state.ignored_errors_joined = PyUnicode_Join(sep, module_state.ignored_errors);
        Py_DECREF(sep);
        if (!module_state.ignored_errors_joined)
            return -1;
    }
    return 0;
}

static int _load_config_from_env(void) {
    const char* use_dict = getenv("COPIUM_USE_DICT_MEMO");
    const char* no_fallback = getenv("COPIUM_NO_MEMO_FALLBACK");

    module_state.memo_mode = (use_dict && use_dict[0]) ? COPIUM_MEMO_DICT : COPIUM_MEMO_NATIVE;
    module_state.on_incompatible = (no_fallback && no_fallback[0]) ? COPIUM_ON_INCOMPATIBLE_RAISE
                                                                   : COPIUM_ON_INCOMPATIBLE_WARN;

    PyObject* parsed = _parse_ignored_errors();
    if (!parsed)
        return -1;

    return _copium_update_suppress_warnings(parsed);
}

int _copium_init(PyObject* module) {
    if (_load_config_from_env() < 0)
        goto error;

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

    module_state.dict_items_descr = PyObject_GetAttrString((PyObject*)&PyDict_Type, "items");
    if (!module_state.dict_items_descr)
        goto error;

    module_state.dict_items_vc = PyVectorcall_Function(module_state.dict_items_descr);
    if (!module_state.dict_items_vc) {
        PyErr_SetString(PyExc_TypeError, "copium: failed to intern dict.items vectorcall");
        goto error;
    }

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

    if (PyObject_SetAttrString(module, "Error", module_state.copy_Error) < 0)
        goto error;
    _init_state.error_attr_ready = 1;

    return 0;

error:
    _copium_cleanup();
    return -1;
}


#endif /* _COPIUM_INIT_C */