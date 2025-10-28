/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov (Bobronium)
 * SPDX-License-Identifier: MIT
 *
 * Patching internals (no standalone module init).
 * Public API is registered onto the "copium" module via:
 *   int _copium_patching_add_api(PyObject* module)
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>

#if PY_VERSION_HEX >= 0x030C0000
/* =========================
 * CPython 3.12+ IMPLEMENTATION
 * ========================= */
#include "cpython/funcobject.h"  // PyFunctionObject, PyVectorcall_Function

static PyObject* KEY_TARGET = NULL;   // "_copium_target"
static PyObject* KEY_SAVED = NULL;    // "_copium_saved_vec"
static PyObject* KEY_WRAPPED = NULL;  // "__wrapped__"
static int g_patching_initialized = 0;

#define SAVED_VEC_CAPSULE_NAME "copium.vectorcall"

static PyObject* fwd_vec(
    PyObject* callable, PyObject* const* args, size_t nargsf, PyObject* kwnames
);

static inline PyObject* make_vec_capsule(vectorcallfunc ptr) {
    return PyCapsule_New((void*)ptr, SAVED_VEC_CAPSULE_NAME, NULL);
}
static inline vectorcallfunc vec_from_capsule(PyObject* cap) {
    void* p = PyCapsule_GetPointer(cap, SAVED_VEC_CAPSULE_NAME);
    if (p == NULL)
        return NULL;
    return (vectorcallfunc)p;
}

static PyObject* fwd_vec(
    PyObject* callable, PyObject* const* args, size_t nargsf, PyObject* kwnames
) {
    if (!PyFunction_Check(callable)) {
        PyErr_SetString(PyExc_RuntimeError, "copium._patch: callable not a PyFunction");
        return NULL;
    }
    PyFunctionObject* fn = (PyFunctionObject*)callable;
    PyObject* f_dict = fn->func_dict;  // borrowed
    if (f_dict == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "copium._patch: func_dict missing");
        return NULL;
    }
    PyObject* target = PyDict_GetItemWithError(f_dict, KEY_TARGET);  // borrowed
    if (target == NULL) {
        if (PyErr_Occurred())
            return NULL;
        PyErr_SetString(PyExc_RuntimeError, "copium._patch: not applied");
        return NULL;
    }
    return _PyObject_Vectorcall(target, args, nargsf, kwnames);
}

static PyObject* m_apply(PyObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "apply(func, target)");
        return NULL;
    }
    PyObject* func = args[0];
    PyObject* target = args[1];

    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "func must be a Python function object");
        return NULL;
    }
    if (!PyCallable_Check(target)) {
        PyErr_SetString(PyExc_TypeError, "target must be callable");
        return NULL;
    }

    PyFunctionObject* fn = (PyFunctionObject*)func;

    if (fn->func_dict == NULL) {
        PyObject* d = PyDict_New();
        if (!d)
            return NULL;
        fn->func_dict = d;  // steals ref
    }

    PyObject* saved_cap = PyDict_GetItemWithError(fn->func_dict, KEY_SAVED);  // borrowed
    if (saved_cap == NULL) {
        if (PyErr_Occurred())
            return NULL;
        vectorcallfunc orig = PyVectorcall_Function(func);
        if (orig == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "copium._patch: function has no vectorcall");
            return NULL;
        }
        PyObject* cap = make_vec_capsule(orig);
        if (!cap)
            return NULL;
        if (PyDict_SetItem(fn->func_dict, KEY_SAVED, cap) < 0) {
            Py_DECREF(cap);
            return NULL;
        }
        Py_DECREF(cap);
    }

    if (PyDict_SetItem(fn->func_dict, KEY_TARGET, target) < 0)
        return NULL;
    if (PyObject_SetAttr(func, KEY_WRAPPED, target) < 0)
        PyErr_Clear();

    PyFunction_SetVectorcall(fn, (vectorcallfunc)fwd_vec);

    Py_RETURN_NONE;
}

static PyObject* m_unapply(PyObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs != 1) {
        PyErr_SetString(PyExc_TypeError, "unapply(func)");
        return NULL;
    }
    PyObject* func = args[0];
    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "func must be a Python function object");
        return NULL;
    }

    PyFunctionObject* fn = (PyFunctionObject*)func;
    if (fn->func_dict == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "copium._patch: not applied (no func_dict)");
        return NULL;
    }

    PyObject* cap = PyDict_GetItemWithError(fn->func_dict, KEY_SAVED);  // borrowed
    if (cap == NULL) {
        if (PyErr_Occurred())
            return NULL;
        PyErr_SetString(PyExc_RuntimeError, "copium._patch: not applied (no saved vec)");
        return NULL;
    }
    vectorcallfunc orig = vec_from_capsule(cap);
    if (orig == NULL)
        return NULL;

    PyFunction_SetVectorcall(fn, orig);

    if (PyDict_DelItem(fn->func_dict, KEY_TARGET) < 0)
        PyErr_Clear();
    if (PyDict_DelItem(fn->func_dict, KEY_SAVED) < 0)
        PyErr_Clear();
    if (PyObject_HasAttr(func, KEY_WRAPPED)) {
        if (PyObject_DelAttr(func, KEY_WRAPPED) < 0)
            PyErr_Clear();
    }

    Py_RETURN_NONE;
}

static PyObject* m_applied(PyObject* self, PyObject* func) {
    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "applied(func): func must be a Python function object");
        return NULL;
    }
    PyFunctionObject* fn = (PyFunctionObject*)func;

    vectorcallfunc cur = PyVectorcall_Function(func);
    if (cur != (vectorcallfunc)fwd_vec)
        Py_RETURN_FALSE;
    if (fn->func_dict == NULL)
        Py_RETURN_FALSE;

    PyObject* v = PyDict_GetItemWithError(fn->func_dict, KEY_TARGET);
    if (v == NULL) {
        if (PyErr_Occurred())
            return NULL;
        Py_RETURN_FALSE;
    }
    Py_RETURN_TRUE;
}

static PyObject* m_get_vectorcall_ptr(PyObject* self, PyObject* func) {
    if (!PyFunction_Check(func)) {
        PyErr_SetString(
            PyExc_TypeError, "get_vectorcall_ptr(func): func must be a Python function object"
        );
        return NULL;
    }
    vectorcallfunc cur = PyVectorcall_Function(func);
    uintptr_t addr = (uintptr_t)(void*)cur;
    return PyLong_FromUnsignedLongLong((unsigned long long)addr);
}

static PyMethodDef patching_methods[] = {
    {"apply", (PyCFunction)(void (*)(void))m_apply, METH_FASTCALL, "apply(func, target)"},
    {"unapply", (PyCFunction)(void (*)(void))m_unapply, METH_FASTCALL, "unapply(func)"},
    {"applied", (PyCFunction)m_applied, METH_O, "applied(func) -> bool"},
    {"get_vectorcall_ptr",
     (PyCFunction)m_get_vectorcall_ptr,
     METH_O,
     "get_vectorcall_ptr(func) -> int"},
    {NULL, NULL, 0, NULL}
};

static int _ensure_inited(void) {
    if (g_patching_initialized)
        return 0;
    KEY_TARGET = PyUnicode_InternFromString("_copium_target");
    KEY_SAVED = PyUnicode_InternFromString("_copium_saved_vec");
    KEY_WRAPPED = PyUnicode_InternFromString("__wrapped__");
    if (!KEY_TARGET || !KEY_SAVED || !KEY_WRAPPED) {
        Py_XDECREF(KEY_TARGET);
        Py_XDECREF(KEY_SAVED);
        Py_XDECREF(KEY_WRAPPED);
        KEY_TARGET = KEY_SAVED = KEY_WRAPPED = NULL;
        return -1;
    }
    g_patching_initialized = 1;
    return 0;
}

int _copium_patching_add_api(PyObject* module) {
    if (_ensure_inited() < 0)
        return -1;
#if PY_VERSION_HEX >= 0x03080000
    if (PyModule_AddFunctions(module, patching_methods) < 0)
        return -1;
#else
    for (PyMethodDef* m = patching_methods; m && m->ml_name; ++m) {
        PyObject* func = PyCFunction_NewEx(m, NULL, PyModule_GetNameObject(module));
        if (!func || PyModule_AddObject(module, m->ml_name, func) < 0) {
            Py_XDECREF(func);
            return -1;
        }
    }
#endif
    return 0;
}

#else /* PY_VERSION_HEX < 3.12 */
/* =========================
 * CPython 3.10/3.11 IMPLEMENTATION
 * ========================= */

/* Globals for legacy branch */
static PyObject* LEGACY_ORIGINAL_CODE = NULL; /* borrowed while applied; owned ref stored */
static PyObject* LEGACY_TEMPLATE_CODE = NULL; /* owned ref to CodeType of template deepcopy */
static PyObject* KEY_WRAPPED = NULL;          /* "__wrapped__" interned unicode */
static int g_patching_initialized = 0;

/* Utility: create kwargs dict { "co_consts": tuple } */
static PyObject* _make_kwargs_for_replace(PyObject* consts_tuple) {
    PyObject* kwargs = PyDict_New();
    if (!kwargs)
        return NULL;
    if (PyDict_SetItemString(kwargs, "co_consts", consts_tuple) < 0) {
        Py_DECREF(kwargs);
        return NULL;
    }
    return kwargs;
}

/* Build a new CodeType from LEGACY_TEMPLATE_CODE with "copium.deepcopy" constant replaced by target callable */
static PyObject* _build_code_with_target(PyObject* target_callable) {
    if (!LEGACY_TEMPLATE_CODE) {
        PyErr_SetString(PyExc_RuntimeError, "copium: template code is not initialized");
        return NULL;
    }
    if (!PyCallable_Check(target_callable)) {
        PyErr_SetString(PyExc_TypeError, "target must be callable");
        return NULL;
    }

    PyObject* co_consts = PyObject_GetAttrString(LEGACY_TEMPLATE_CODE, "co_consts");
    if (!co_consts)
        return NULL;

    PyObject* list_obj = PySequence_List(co_consts);
    Py_DECREF(co_consts);
    if (!list_obj)
        return NULL;

    PyObject* sentinel = PyUnicode_FromString("copium.deepcopy");
    if (!sentinel) {
        Py_DECREF(list_obj);
        return NULL;
    }

    Py_ssize_t n = PyList_GET_SIZE(list_obj);
    Py_ssize_t idx = -1;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* item = PyList_GET_ITEM(list_obj, i); /* borrowed */
        int is_eq = PyObject_RichCompareBool(item, sentinel, Py_EQ);
        if (is_eq == -1) {
            Py_DECREF(sentinel);
            Py_DECREF(list_obj);
            return NULL;
        }
        if (is_eq == 1) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        int found_target = 0;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = PyList_GET_ITEM(list_obj, i);
            int is_eq = (item == target_callable);
            if (is_eq) {
                found_target = 1;
                break;
            }
        }
        if (!found_target) {
            PyObject* repr = PyObject_Repr(list_obj);
            if (!repr)
                repr = PyUnicode_FromString("<unrepr>");
            PyErr_Format(
                PyExc_RuntimeError, "Couldn't find constant to replace in %U with target", repr
            );
            Py_XDECREF(repr);
            Py_DECREF(sentinel);
            Py_DECREF(list_obj);
            return NULL;
        }
    } else {
        if (PyList_SetItem(list_obj, idx, target_callable) < 0) {
            Py_DECREF(sentinel);
            Py_DECREF(list_obj);
            return NULL;
        }
        Py_INCREF(target_callable); /* keep caller ownership */
    }

    PyObject* new_consts = PyList_AsTuple(list_obj);
    Py_DECREF(list_obj);
    Py_DECREF(sentinel);
    if (!new_consts)
        return NULL;

    PyObject* replace = PyObject_GetAttrString(LEGACY_TEMPLATE_CODE, "replace");
    if (!replace) {
        Py_DECREF(new_consts);
        return NULL;
    }

    PyObject* kwargs = _make_kwargs_for_replace(new_consts);
    Py_DECREF(new_consts);
    if (!kwargs) {
        Py_DECREF(replace);
        return NULL;
    }

    PyObject* new_code = PyObject_Call(replace, PyTuple_New(0), kwargs);
    Py_DECREF(replace);
    Py_DECREF(kwargs);
    return new_code; /* new reference or NULL on error */
}

static PyObject* m_apply(PyObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "apply(func, target)");
        return NULL;
    }
    PyObject* func = args[0];
    PyObject* target = args[1];

    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "func must be a Python function object");
        return NULL;
    }
    if (!PyCallable_Check(target)) {
        PyErr_SetString(PyExc_TypeError, "target must be callable");
        return NULL;
    }

    PyObject* code_attr = PyObject_GetAttrString(func, "__code__");
    if (!code_attr)
        return NULL;

    if (LEGACY_ORIGINAL_CODE != NULL && code_attr != LEGACY_ORIGINAL_CODE) {
        Py_DECREF(code_attr);
        PyErr_SetString(PyExc_AssertionError, "Function was already applied");
        return NULL;
    }

    if (LEGACY_ORIGINAL_CODE == NULL) {
        LEGACY_ORIGINAL_CODE = code_attr;
        Py_INCREF(LEGACY_ORIGINAL_CODE);
    } else {
        Py_DECREF(code_attr);
    }

    PyObject* new_code = _build_code_with_target(target);
    if (!new_code)
        return NULL;

    int rc = PyObject_SetAttrString(func, "__code__", new_code);
    Py_DECREF(new_code);
    if (rc < 0)
        return NULL;

    if (!KEY_WRAPPED)
        KEY_WRAPPED = PyUnicode_InternFromString("__wrapped__");
    if (KEY_WRAPPED && PyObject_SetAttr(func, KEY_WRAPPED, target) < 0) {
        PyErr_Clear();
    }

    Py_RETURN_NONE;
}

static PyObject* m_unapply(PyObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs != 1) {
        PyErr_SetString(PyExc_TypeError, "unapply(func)");
        return NULL;
    }
    PyObject* func = args[0];
    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "func must be a Python function object");
        return NULL;
    }

    if (LEGACY_ORIGINAL_CODE == NULL) {
        PyErr_SetString(PyExc_AssertionError, "Target was not applied");
        return NULL;
    }

    PyObject* cur_code = PyObject_GetAttrString(func, "__code__");
    if (!cur_code)
        return NULL;

    int already_unapplied = (cur_code == LEGACY_ORIGINAL_CODE);
    (void)already_unapplied;
    Py_DECREF(cur_code);

    if (PyObject_SetAttrString(func, "__code__", LEGACY_ORIGINAL_CODE) < 0)
        return NULL;

    if (!KEY_WRAPPED)
        KEY_WRAPPED = PyUnicode_InternFromString("__wrapped__");
    if (KEY_WRAPPED && PyObject_HasAttr(func, KEY_WRAPPED)) {
        if (PyObject_DelAttr(func, KEY_WRAPPED) < 0)
            PyErr_Clear();
    }

    Py_DECREF(LEGACY_ORIGINAL_CODE);
    LEGACY_ORIGINAL_CODE = NULL;

    Py_RETURN_NONE;
}

static PyObject* m_applied(PyObject* self, PyObject* func) {
    if (!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "applied(func): func must be a Python function object");
        return NULL;
    }
    if (LEGACY_ORIGINAL_CODE == NULL)
        Py_RETURN_FALSE;

    PyObject* cur_code = PyObject_GetAttrString(func, "__code__");
    if (!cur_code)
        return NULL;

    int is_p = (cur_code != LEGACY_ORIGINAL_CODE);
    Py_DECREF(cur_code);
    if (is_p)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

/* get_vectorcall_ptr(func) -> int (still useful as a debug helper) */
static PyObject* m_get_vectorcall_ptr(PyObject* self, PyObject* func) {
    if (!PyFunction_Check(func)) {
        PyErr_SetString(
            PyExc_TypeError, "get_vectorcall_ptr(func): func must be a Python function object"
        );
        return NULL;
    }
    vectorcallfunc cur = PyVectorcall_Function(func);
    uintptr_t addr = (uintptr_t)(void*)cur;
    return PyLong_FromUnsignedLongLong((unsigned long long)addr);
}

static PyMethodDef patching_methods[] = {
    {"apply", (PyCFunction)(void (*)(void))m_apply, METH_FASTCALL, "apply(func, target)"},
    {"unapply", (PyCFunction)(void (*)(void))m_unapply, METH_FASTCALL, "unapply(func)"},
    {"applied", (PyCFunction)m_applied, METH_O, "applied(func) -> bool"},
    {"get_vectorcall_ptr",
     (PyCFunction)m_get_vectorcall_ptr,
     METH_O,
     "get_vectorcall_ptr(func) -> int"},
    {NULL, NULL, 0, NULL}
};

static int _init_legacy_template_code(void) {
    static const char* src =
        "def deepcopy(x, memo=None, _nil=[]):\n"
        "    return \"copium.deepcopy\"(x, memo)\n";

    PyObject* globals = PyDict_New();
    PyObject* locals = globals;
    if (!globals)
        return -1;

    PyObject* builtins = PyEval_GetBuiltins(); /* borrowed */
    if (builtins && PyDict_SetItemString(globals, "__builtins__", builtins) < 0) {
        Py_DECREF(globals);
        return -1;
    }
    PyObject* warnings = PyImport_ImportModule("warnings");
    PyObject* old_filters = PyObject_GetAttrString(warnings, "filters");

    PyObject* filters_copy = PySequence_List(old_filters);  // shallow copy
    PyObject* ignore =
        PyObject_CallMethod(warnings, "simplefilter", "sO", "ignore", PyExc_SyntaxWarning);
    Py_XDECREF(ignore);

    PyObject* res = PyRun_StringFlags(src, Py_file_input, globals, locals, NULL);
    if (!res) {
        Py_DECREF(globals);
        return -1;
    }
    Py_DECREF(res);
    if (filters_copy) {
        PyObject_SetAttrString(warnings, "filters", filters_copy);
        Py_DECREF(filters_copy);
    }

    Py_XDECREF(old_filters);
    Py_XDECREF(warnings);
    PyObject* deepcopy_fn = PyDict_GetItemString(locals, "deepcopy"); /* borrowed */
    if (!deepcopy_fn) {
        Py_DECREF(globals);
        PyErr_SetString(PyExc_RuntimeError, "Failed to locate synthesized deepcopy()");
        return -1;
    }

    PyObject* code = PyObject_GetAttrString(deepcopy_fn, "__code__");
    if (!code) {
        Py_DECREF(globals);
        return -1;
    }

    LEGACY_TEMPLATE_CODE = code; /* keep owned ref */
    Py_DECREF(globals);
    return 0;
}

static int _ensure_inited(void) {
    if (g_patching_initialized)
        return 0;
    KEY_WRAPPED = PyUnicode_InternFromString("__wrapped__");
    if (!KEY_WRAPPED)
        return -1;
    if (_init_legacy_template_code() < 0)
        return -1;
    g_patching_initialized = 1;
    return 0;
}

int _copium_patching_add_api(PyObject* module) {
    if (_ensure_inited() < 0)
        return -1;
#if PY_VERSION_HEX >= 0x03080000
    if (PyModule_AddFunctions(module, patching_methods) < 0)
        return -1;
#else
    for (PyMethodDef* m = patching_methods; m && m->ml_name; ++m) {
        PyObject* func = PyCFunction_NewEx(m, NULL, PyModule_GetNameObject(module));
        if (!func || PyModule_AddObject(module, m->ml_name, func) < 0) {
            Py_XDECREF(func);
            return -1;
        }
    }
#endif
    return 0;
}
#endif /* version branch */
