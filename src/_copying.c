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
#if defined(__APPLE__) || defined(__linux__)
    #include <pthread.h>
#endif
#if defined(_WIN32)
    #include <windows.h>
#endif

#include "Python.h"

/* _PyDict_NewPresized */
#if PY_VERSION_HEX < PY_VERSION_3_11_HEX
    #include "dictobject.h"
#else
    #include "pycore_dict.h"
#endif

/* _PySet_NextEntry() */
#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
    #include "setobject.h"
#else
    #include "pycore_setobject.h"
#endif

/* ------------------------------ Public API ---------------------------------
 */

PyObject* py_deepcopy(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    PyObject* obj = NULL;
    PyObject* memo_arg = Py_None;

    if (!kwnames || PyTuple_GET_SIZE(kwnames) == 0) {
        if (UNLIKELY(nargs < 1)) {
            PyErr_Format(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
            return NULL;
        }
        if (UNLIKELY(nargs > 2)) {
            PyErr_Format(
                PyExc_TypeError,
                "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                nargs
            );
            return NULL;
        }
        obj = args[0];
        memo_arg = (nargs == 2) ? args[1] : Py_None;
        goto have_args;
    }

    const Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
    if (kwcount == 1) {
        PyObject* kw0 = PyTuple_GET_ITEM(kwnames, 0);
        const int is_memo =
            PyUnicode_Check(kw0) && PyUnicode_CompareWithASCIIString(kw0, "memo") == 0;

        if (is_memo) {
            if (UNLIKELY(nargs < 1)) {
                PyErr_Format(
                    PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'"
                );
                return NULL;
            }
            if (UNLIKELY(nargs > 2)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                    nargs
                );
                return NULL;
            }
            if (UNLIKELY(nargs == 2)) {
                PyErr_SetString(
                    PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'"
                );
                return NULL;
            }
            obj = args[0];
            memo_arg = args[nargs + 0];
            goto have_args;
        }
    }

    {
        Py_ssize_t i;
        int seen_memo_kw = 0;

        if (UNLIKELY(nargs > 2)) {
            PyErr_Format(
                PyExc_TypeError,
                "deepcopy() takes from 1 to 2 positional arguments but %zd were given",
                nargs
            );
            return NULL;
        }

        if (nargs >= 1)
            obj = args[0];
        if (nargs == 2)
            memo_arg = args[1];

        const Py_ssize_t kwc = PyTuple_GET_SIZE(kwnames);
        for (i = 0; i < kwc; i++) {
            PyObject* name = PyTuple_GET_ITEM(kwnames, i);
            PyObject* val = args[nargs + i];

            if (!(PyUnicode_Check(name))) {
                PyErr_SetString(PyExc_TypeError, "deepcopy() keywords must be strings");
                return NULL;
            }

            if (PyUnicode_CompareWithASCIIString(name, "x") == 0) {
                if (UNLIKELY(obj != NULL)) {
                    PyErr_SetString(
                        PyExc_TypeError, "deepcopy() got multiple values for argument 'x'"
                    );
                    return NULL;
                }
                obj = val;
                continue;
            }

            if (PyUnicode_CompareWithASCIIString(name, "memo") == 0) {
                if (UNLIKELY(seen_memo_kw || nargs == 2)) {
                    PyErr_SetString(
                        PyExc_TypeError, "deepcopy() got multiple values for argument 'memo'"
                    );
                    return NULL;
                }
                memo_arg = val;
                seen_memo_kw = 1;
                continue;
            }

            PyErr_Format(
                PyExc_TypeError, "deepcopy() got an unexpected keyword argument '%U'", name
            );
            return NULL;
        }

        if (UNLIKELY(obj == NULL)) {
            PyErr_Format(PyExc_TypeError, "deepcopy() missing 1 required positional argument: 'x'");
            return NULL;
        }
    }

have_args:
    if (memo_arg == Py_None) {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            return Py_NewRef(obj);
        }
        PyObject* memo_local = get_tss_memo();
        if (!memo_local)
            return NULL;
        MemoObject* memo = (MemoObject*)memo_local;

        PyObject* result = deepcopy(obj, memo);
        cleanup_tss_memo(memo, memo_local);
        return result;
    }

    PyObject* result = NULL;

    if (Py_TYPE(memo_arg) == &Memo_Type) {
        MemoObject* memo = (MemoObject*)memo_arg;
        Py_INCREF(memo_arg);
        result = deepcopy(obj, memo);
        Py_DECREF(memo_arg);
        return result;
    }

    else {
        /* deepcopy_py handles version-specific ordering of immutable check vs memo lookup */
        Py_INCREF(memo_arg);
        PyObject* memo = memo_arg;
        PyObject* keep_list = NULL; /* lazily created on first append */

        result = deepcopy_legacy(obj, memo, &keep_list);

        Py_XDECREF(keep_list);
        Py_DECREF(memo);
        return result;
    }
}

/* -------------------------------- Utilities -------------------------------- */

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

PyObject* py_replicate(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;

    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "replicate(obj, n, /, *, compile_after=20)");
        return NULL;
    }

    PyObject* obj = args[0];

    long n_long = PyLong_AsLong(args[1]);
    if (n_long == -1 && PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "n must be >= 0");
        return NULL;
    }
    Py_ssize_t n = (Py_ssize_t)n_long;

    int duper_available = (module_state.create_precompiler_reconstructor != NULL);

    int compile_after = 20;
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        if (kwcount > 1) {
            PyErr_SetString(PyExc_TypeError, "replicate accepts only 'compile_after' keyword");
            return NULL;
        }
        if (kwcount == 1) {
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, 0);
            int is_compile_after = PyUnicode_Check(kwname) &&
                (PyUnicode_CompareWithASCIIString(kwname, "compile_after") == 0);
            if (!is_compile_after) {
                PyErr_SetString(
                    PyExc_TypeError, "unknown keyword; only 'compile_after' is supported"
                );
                return NULL;
            }
            if (!duper_available) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "replicate(): 'compile_after' requires duper.snapshots; it is not available"
                );
                return NULL;
            }
            PyObject* kwval = args[nargs + 0];
            long ca = PyLong_AsLong(kwval);
            if (ca == -1 && PyErr_Occurred())
                return NULL;
            if (ca < 0) {
                PyErr_SetString(PyExc_ValueError, "compile_after must be >= 0");
                return NULL;
            }
            compile_after = (int)ca;
        }
    }

    if (n == 0)
        return PyList_New(0);

    {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            PyObject* out = PyList_New(n);
            if (!out)
                return NULL;
            for (Py_ssize_t i = 0; i < n; i++) {
                {
                    PyObject* copy_i = Py_NewRef(obj);
                    PyList_SET_ITEM(out, i, copy_i);
                }
            }
            return out;
        }
    }

    {
        PinObject* pin = _duper_lookup_pin_for_object(obj);
        if (pin) {
            PyObject* factory = pin->factory;
            if (UNLIKELY(!factory || !PyCallable_Check(factory))) {
                PyErr_SetString(PyExc_RuntimeError, "pinned object has no valid factory");
                return NULL;
            }
            PyObject* out = build_list_by_calling_noargs(factory, n);
            if (out)
                pin->hits += (uint64_t)n;
            return out;
        }
    }

    if (!duper_available || n <= (Py_ssize_t)compile_after) {
        PyObject* out = PyList_New(n);
        if (!out)
            return NULL;

        PyObject* memo_local = get_tss_memo();
        if (!memo_local)
            return NULL;
        MemoObject* memo = (MemoObject*)memo_local;

        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* copy_i = deepcopy(obj, memo);

            if (!cleanup_tss_memo(memo, memo_local)) {
                PyObject* memo_local = get_tss_memo();
                if (!memo_local) {
                    Py_DECREF(out);
                    return NULL;
                }
                memo = (MemoObject*)memo_local;
            }

            if (!copy_i) {
                Py_DECREF(out);
                return NULL;
            }
            PyList_SET_ITEM(out, i, copy_i);
        }
        return out;
    }

    {
        PyObject* cpr = module_state.create_precompiler_reconstructor;
        if (UNLIKELY(!cpr || !PyCallable_Check(cpr))) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "duper.snapshots.create_precompiler_reconstructor is not callable"
            );
            return NULL;
        }

        PyObject* reconstructor = PyObject_CallOneArg(cpr, obj);
        if (!reconstructor)
            return NULL;

        if (UNLIKELY(!PyCallable_Check(reconstructor))) {
            Py_DECREF(reconstructor);
            PyErr_SetString(PyExc_TypeError, "reconstructor must be callable (FunctionType)");
            return NULL;
        }

        PyObject* out = build_list_by_calling_noargs(reconstructor, n);
        Py_DECREF(reconstructor);
        return out;
    }
}

PyObject* py_repeatcall(
    PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
) {
    (void)self;
    if (UNLIKELY(nargs != 2)) {
        PyErr_SetString(PyExc_TypeError, "repeatcall(function, size, /)");
        return NULL;
    }
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        PyErr_SetString(PyExc_TypeError, "repeatcall() takes no keyword arguments");
        return NULL;
    }

    PyObject* func = args[0];
    if (UNLIKELY(!PyCallable_Check(func))) {
        PyErr_SetString(PyExc_TypeError, "function must be callable");
        return NULL;
    }

    long n_long = PyLong_AsLong(args[1]);
    if (n_long == -1 && PyErr_Occurred())
        return NULL;
    if (n_long < 0) {
        PyErr_SetString(PyExc_ValueError, "size must be >= 0");
        return NULL;
    }

    Py_ssize_t n = (Py_ssize_t)n_long;
    return build_list_by_calling_noargs(func, n);
}

/* ----------------------------- Shallow reconstruct helper ------------------ */


/* ----------------------------- Module boilerplate --------------------------
 */

extern PyObject* py_copy(PyObject* self, PyObject* obj);
#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
extern PyObject* py_replace(
    PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
);
#endif

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
