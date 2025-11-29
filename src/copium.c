/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium - Fast, full-native deepcopy for Python
 *
 * Main module providing:
 *   - copy(obj)                  - shallow copy
 *   - deepcopy(obj, memo=None)   - deep copy
 *   - replace(obj, **changes)    - replace fields (Python >= 3.13)
 *   - Error                      - copy.Error exception
 *
 * Submodules:
 *   - copium.patch        - stdlib patching (enable, disable, enabled)
 *   - copium.extra        - batch utilities (replicate, repeatcall)
 *   - copium.__about__    - version information
 *   - copium._experimental - pin API (when duper.snapshots available)
 */

/* Enable GNU extensions for pthread_getattr_np on Linux */
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE 1
#endif

#include "_common.h"

/* ========================================================================== */
/*                         Internal Implementation                            */
/* ========================================================================== */

#include "_state.c"
#include "_dict_iter.c"
#include "_type_checks.c"
#include "_recursion_guard.c"
#include "_reduce_helpers.c"
#include "_memo.c"
#include "_deepcopy.c"
#include "_memo_legacy.c"
#include "_deepcopy_legacy.c"
#include "_copy.c"
#include "_pinning.c"
#include "_patching.c"
#include "_init.c"

/* ========================================================================== */
/*                            Submodules                                      */
/* ========================================================================== */

#include "copium_patch.c"
#include "copium_extra.c"
#include "copium___about__.c"
#include "copium__experimental.c"

/* ========================================================================== */
/*                              Main API                                      */
/* ========================================================================== */

PyObject* py_copy(PyObject* self, PyObject* obj) {
    (void)self;

    {
        PyTypeObject* tp = Py_TYPE(obj);
        if (is_atomic_immutable(tp)) {
            return Py_NewRef(obj);
        }
    }

    if (PySlice_Check(obj))
        return Py_NewRef(obj);
    if (PyFrozenSet_CheckExact(obj))
        return Py_NewRef(obj);

    if (PyType_IsSubtype(Py_TYPE(obj), &PyType_Type))
        return Py_NewRef(obj);

    if (is_empty_initializable(obj)) {
        PyObject* fresh = make_empty_same_type(obj);
        if (fresh == Py_None)
            Py_DECREF(fresh);
        else
            return fresh;
    }

    {
        PyObject* maybe = try_stdlib_mutable_copy(obj);
        if (!maybe)
            return NULL;
        if (maybe != Py_None)
            return maybe;
        Py_DECREF(maybe);
    }

    {
        PyObject* cp = PyObject_GetAttrString(obj, "__copy__");
        if (cp) {
            PyObject* out = PyObject_CallNoArgs(cp);
            Py_DECREF(cp);
            return out;
        }
        PyErr_Clear();
    }

    PyTypeObject* obj_type = Py_TYPE(obj);
    PyObject* reduce_result = try_reduce_via_registry(obj, obj_type);
    if (!reduce_result) {
        if (PyErr_Occurred())
            return NULL;
        reduce_result = call_reduce_method_preferring_ex(obj);
        if (!reduce_result)
            return NULL;
    }

    PyObject *constructor = NULL, *args = NULL, *state = NULL, *listiter = NULL, *dictiter = NULL;
    int unpack_result =
        validate_reduce_tuple(reduce_result, &constructor, &args, &state, &listiter, &dictiter);
    if (unpack_result == REDUCE_ERROR) {
        Py_DECREF(reduce_result);
        return NULL;
    }
    if (unpack_result == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(obj);
    }

    PyObject* out = NULL;
    if (PyTuple_GET_SIZE(args) == 0) {
        out = PyObject_CallNoArgs(constructor);
    } else {
        out = PyObject_CallObject(constructor, args);
    }
    if (!out) {
        Py_DECREF(reduce_result);
        return NULL;
    }

    if ((state && state != Py_None) || (listiter && listiter != Py_None) ||
        (dictiter && dictiter != Py_None)) {
        PyObject* applied = reconstruct_state(
            out,
            state ? state : Py_None,
            listiter ? listiter : Py_None,
            dictiter ? dictiter : Py_None
        );
        if (!applied) {
            Py_DECREF(out);
            Py_DECREF(reduce_result);
            return NULL;
        }
        Py_DECREF(applied);
    }

    Py_DECREF(reduce_result);
    return out;
}

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

#if PY_VERSION_HEX >= PY_VERSION_3_13_HEX
PyObject* py_replace(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;
    if (UNLIKELY(nargs == 0)) {
        PyErr_SetString(PyExc_TypeError, "replace() missing 1 required positional argument: 'obj'");
        return NULL;
    }
    if (UNLIKELY(nargs > 1)) {
        PyErr_Format(
            PyExc_TypeError, "replace() takes 1 positional argument but %zd were given", nargs
        );
        return NULL;
    }
    PyObject* obj = args[0];
    PyObject* cls = (PyObject*)Py_TYPE(obj);

    PyObject* func = PyObject_GetAttrString(cls, "__replace__");
    if (!func) {
        PyErr_Clear();
        PyErr_Format(
            PyExc_TypeError, "replace() does not support %.200s objects", Py_TYPE(obj)->tp_name
        );
        return NULL;
    }
    if (!PyCallable_Check(func)) {
        Py_DECREF(func);
        PyErr_SetString(PyExc_TypeError, "__replace__ is not callable");
        return NULL;
    }

    PyObject* posargs = PyTuple_New(1);
    if (!posargs) {
        Py_DECREF(func);
        return NULL;
    }
    Py_INCREF(obj);
    PyTuple_SET_ITEM(posargs, 0, obj);

    PyObject* kwargs = NULL;
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        kwargs = PyDict_New();
        if (!kwargs) {
            Py_DECREF(func);
            Py_DECREF(posargs);
            return NULL;
        }
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            PyObject* key = PyTuple_GET_ITEM(kwnames, i);
            PyObject* val = args[nargs + i];
            if (PyDict_SetItem(kwargs, key, val) < 0) {
                Py_DECREF(func);
                Py_DECREF(posargs);
                Py_DECREF(kwargs);
                return NULL;
            }
        }
    }

    PyObject* out = PyObject_Call(func, posargs, kwargs);
    Py_DECREF(func);
    Py_DECREF(posargs);
    Py_XDECREF(kwargs);
    return out;
}
#endif

/* ========================================================================== */
/*                         Module Definition                                  */
/* ========================================================================== */

static PyMethodDef main_methods[] = {
    {"copy",
     (PyCFunction)py_copy,
     METH_O,
     PyDoc_STR(
         "copy(obj, /)\n--\n\n"
         "Return a shallow copy of obj.\n\n"
         ":param x: object to copy.\n"
         ":return: shallow copy of the `x`."
     )},
    {"deepcopy",
     (PyCFunction)(void*)py_deepcopy,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "deepcopy(x, memo=None, /)\n--\n\n"
         "Return a deep copy of obj.\n\n"
         ":param x: object to deepcopy\n"
         ":param memo: treat as opaque.\n"
         ":return: deep copy of the `x`."
     )},
#if PY_VERSION_HEX >= 0x030D0000
    {"replace",
     (PyCFunction)(void*)py_replace,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "replace(obj, /, **changes)\n--\n\n"
         "Creates a new object of the same type as obj, replacing fields with values from changes."
     )},
#endif
    {NULL, NULL, 0, NULL}
};

static int copium_exec(PyObject* module);

static struct PyModuleDef_Slot main_slots[] = {
#ifdef Py_GIL_DISABLED
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {Py_mod_exec, copium_exec},
    {0, NULL}
};

static struct PyModuleDef main_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium",
    "Fast, full-native deepcopy with reduce protocol and keepalive memo.",
    0,
    main_methods,
    main_slots,
    NULL,
    NULL,
    NULL
};

/* ========================================================================== */
/*                         Submodule Helpers                                  */
/* ========================================================================== */

/**
 * Add a submodule to parent and register in sys.modules.
 *
 * NOTE: On success, reference to submodule is stolen.
 * On failure, submodule is decref'd.
 */
static int _add_submodule(PyObject* parent, const char* name, PyObject* submodule) {
    if (!submodule)
        return -1;

    PyObject* parent_name = PyModule_GetNameObject(parent);
    if (!parent_name) {
        Py_DECREF(submodule);
        return -1;
    }

    PyObject* full_name = PyUnicode_FromFormat("%U.%s", parent_name, name);
    Py_DECREF(parent_name);
    if (!full_name) {
        Py_DECREF(submodule);
        return -1;
    }

    if (PyObject_SetAttrString(submodule, "__name__", full_name) < 0) {
        Py_DECREF(full_name);
        Py_DECREF(submodule);
        return -1;
    }

    PyObject* sys_modules = PyImport_GetModuleDict();
    if (!sys_modules) {
        Py_DECREF(full_name);
        Py_DECREF(submodule);
        return -1;
    }

    if (PyDict_SetItem(sys_modules, full_name, submodule) < 0) {
        Py_DECREF(full_name);
        Py_DECREF(submodule);
        return -1;
    }
    Py_DECREF(full_name);

    if (PyModule_AddObject(parent, name, submodule) < 0) {
        Py_DECREF(submodule);
        return -1;
    }

    return 0;
}

/* ========================================================================== */
/*                         Module Initialization                              */
/* ========================================================================== */

PyMODINIT_FUNC PyInit_copium(void) {
    return PyModuleDef_Init(&main_module_def);
}

static int copium_exec(PyObject* module) {
    /* Initialize internal state */
    if (_copium_init(module) < 0)
        return -1;

    /* Create and attach extra submodule */
    PyObject* extra_module = PyModule_Create(&extra_module_def);
    if (_add_submodule(module, "extra", extra_module) < 0)
        return -1;

    /* Create and attach patch submodule */
    PyObject* patch_module = PyModule_Create(&patch_module_def);
    if (_add_submodule(module, "patch", patch_module) < 0)
        return -1;

    /* Add low-level patching API to patch module */
    if (_copium_patching_add_api(patch_module) < 0)
        return -1;

    /* Conditionally create experimental submodule */
    if (_copium_duper_available()) {
        PyObject* experimental_module = PyModule_Create(&experimental_module_def);
        if (_add_submodule(module, "_experimental", experimental_module) < 0)
            return -1;
    }

    /* Build and attach __about__ submodule */
    if (_build_about_module(module, _add_submodule) < 0)
        return -1;

    return 0;
}