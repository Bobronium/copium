/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Public module definition & API glue for the copium extension.
 *
 * Module structure:
 *   copium (main):
 *     - copy(obj)
 *     - deepcopy(x, memo=None)
 *     - replace(obj, /, **changes)  [Python >= 3.13]
 *
 *   copium.extra:
 *     - replicate(obj, n, /, *, compile_after=20)
 *     - repeatcall(function, size, /)
 *
 *   copium.patch:
 *     - enable() / disable() / enabled()
 *     - apply() / unapply() / applied() / get_vectorcall_ptr()
 *
 *   copium._experimental (only when duper.snapshots available):
 *     - pin(obj)
 *     - unpin(obj, *, strict=False)
 *     - pinned(obj)
 *     - clear_pins()
 *     - get_pins()
 */
#define Py_BUILD_CORE_MODULE

#define PY_VERSION_3_11_HEX 0x030B0000
#define PY_VERSION_3_12_HEX 0x030C0000
#define PY_VERSION_3_13_HEX 0x030D0000
#define PY_VERSION_3_14_HEX 0x030E0000

#define PY_SSIZE_T_CLEAN

/* Enable GNU extensions so pthread_getattr_np is declared on Linux */
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE 1
#endif

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#ifndef LIKELY
    #if defined(__GNUC__) || defined(__clang__)
        #define LIKELY(x) __builtin_expect(!!(x), 1)
        #define UNLIKELY(x) __builtin_expect(!!(x), 0)
    #else
        #define LIKELY(x) (x)
        #define UNLIKELY(x) (x)
    #endif
#endif

#ifndef ALWAYS_INLINE
    #if (defined(__GNUC__) || defined(__clang__)) && defined(__OPTIMIZE__)
        #define ALWAYS_INLINE inline __attribute__((always_inline))
    #elif defined(_MSC_VER)
        #define ALWAYS_INLINE __forceinline
    #else
        #define ALWAYS_INLINE inline
    #endif
#endif

#include "_memo.c"
#include "_pinning.c"
#include "_copying.c"
#include "_patching.c"

/* ===================== Utilities shared locally =========================== */

static PyObject* _get_attr_str(PyObject* obj, const char* name) {
    return PyObject_GetAttrString(obj, name);
}

static int _truthy(PyObject* obj) {
    int result = PyObject_IsTrue(obj);
    Py_DECREF(obj);
    return result;
}

/* ===================== Patch module: enable/disable/enabled ================ */

static PyObject* py_enable(PyObject* self, PyObject* noargs) {
    (void)noargs;
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* py_deepcopy_object = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!py_deepcopy_object)
        return NULL;

    if (!PyFunction_Check(py_deepcopy_object)) {
        Py_DECREF(py_deepcopy_object);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    /* Get applied() from this module (copium.patch) */
    PyObject* function_applied = _get_attr_str(self, "applied");
    if (!function_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* is_applied = PyObject_CallOneArg(function_applied, py_deepcopy_object);
    Py_DECREF(function_applied);
    if (!is_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    int already = _truthy(is_applied);
    if (already < 0) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }
    if (already) {
        Py_DECREF(py_deepcopy_object);
        Py_RETURN_FALSE;
    }

    /* Get native deepcopy from main copium module */
    PyObject* copium_main = PyImport_ImportModule("copium");
    if (!copium_main) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }
    PyObject* native_deepcopy = _get_attr_str(copium_main, "deepcopy");
    Py_DECREF(copium_main);
    if (!native_deepcopy) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    /* Get apply() from this module */
    PyObject* function_apply = _get_attr_str(self, "apply");
    if (!function_apply) {
        Py_DECREF(py_deepcopy_object);
        Py_DECREF(native_deepcopy);
        return NULL;
    }

    PyObject* result =
        PyObject_CallFunction(function_apply, "OO", py_deepcopy_object, native_deepcopy);
    Py_DECREF(function_apply);
    Py_DECREF(py_deepcopy_object);
    Py_DECREF(native_deepcopy);
    if (!result)
        return NULL;
    Py_DECREF(result);

    Py_RETURN_TRUE;
}

static PyObject* py_disable(PyObject* self, PyObject* noargs) {
    (void)noargs;
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* py_deepcopy_object = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!py_deepcopy_object)
        return NULL;

    if (!PyFunction_Check(py_deepcopy_object)) {
        Py_DECREF(py_deepcopy_object);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    /* Get applied() from this module */
    PyObject* function_applied = _get_attr_str(self, "applied");
    if (!function_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* is_applied = PyObject_CallOneArg(function_applied, py_deepcopy_object);
    Py_DECREF(function_applied);
    if (!is_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    int active = _truthy(is_applied);
    if (active < 0) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }
    if (!active) {
        Py_DECREF(py_deepcopy_object);
        Py_RETURN_FALSE;
    }

    /* Get unapply() from this module */
    PyObject* function_unapply = _get_attr_str(self, "unapply");
    if (!function_unapply) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* result = PyObject_CallFunction(function_unapply, "O", py_deepcopy_object);
    Py_DECREF(function_unapply);
    Py_DECREF(py_deepcopy_object);
    if (!result)
        return NULL;
    Py_DECREF(result);

    Py_RETURN_TRUE;
}

static PyObject* py_enabled(PyObject* self, PyObject* noargs) {
    (void)noargs;
    PyObject* module_copy = PyImport_ImportModule("copy");
    if (!module_copy)
        return NULL;

    PyObject* py_deepcopy_object = PyObject_GetAttrString(module_copy, "deepcopy");
    Py_DECREF(module_copy);
    if (!py_deepcopy_object)
        return NULL;

    if (!PyFunction_Check(py_deepcopy_object)) {
        Py_DECREF(py_deepcopy_object);
        PyErr_SetString(PyExc_TypeError, "copy.deepcopy is not a Python function");
        return NULL;
    }

    /* Get applied() from this module */
    PyObject* function_applied = _get_attr_str(self, "applied");
    if (!function_applied) {
        Py_DECREF(py_deepcopy_object);
        return NULL;
    }

    PyObject* is_applied = PyObject_CallOneArg(function_applied, py_deepcopy_object);
    Py_DECREF(function_applied);
    Py_DECREF(py_deepcopy_object);
    if (!is_applied)
        return NULL;

    int active = _truthy(is_applied);
    if (active < 0)
        return NULL;
    if (active)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

/* ===================== Method tables for submodules ======================== */

/* Main module: copy/deepcopy/replace */
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

/* Utils submodule: replicate/repeatcall */
static PyMethodDef extra_methods[] = {
    {"replicate",
     (PyCFunction)(void*)py_replicate,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "replicate(obj, n, /, *, compile_after=20)\n--\n\n"
         "Returns n copies of the object in a list.\n\n"
         "Equivalent of [deepcopy(obj) for _ in range(n)], but faster."
     )},
    {"repeatcall",
     (PyCFunction)(void*)py_repeatcall,
     METH_FASTCALL | METH_KEYWORDS,
     PyDoc_STR(
         "repeatcall(function, size, /)\n--\n\n"
         "Call function repeatedly size times and return the list of results.\n\n"
         "Equivalent of [function() for _ in range(size)], but faster."
     )},
    {NULL, NULL, 0, NULL}
};

/* Patch submodule: enable/disable/enabled + patching functions */
static PyMethodDef patch_methods[] = {
    {"enable",
     (PyCFunction)py_enable,
     METH_NOARGS,
     PyDoc_STR(
         "enable()\n--\n\n"
         "Patch copy.deepcopy to forward to copium.deepcopy.\n\n"
         ":return: True if copium was enabled, False if it was already enabled."
     )},
    {"disable",
     (PyCFunction)py_disable,
     METH_NOARGS,
     PyDoc_STR(
         "disable()\n--\n\n"
         "Undo enable(): restore original copy.deepcopy if applied.\n\n"
         ":return: True if copium was disabled, False if it was already disabled."
     )},
    {"enabled",
     (PyCFunction)py_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "enabled()\n--\n\n"
         "Return True if copy.deepcopy is currently applied to copium.\n\n"
         ":return: Whether copium is currently enabled."
     )},
    {NULL, NULL, 0, NULL}
};

/* Experimental submodule: pin API (conditional) */
static PyMethodDef experimental_methods[] = {
    {"pin", (PyCFunction)py_pin, METH_O, PyDoc_STR("pin(obj, /)\n--\n\nReturn a Pin for obj.")},
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

/* ===================== Module definitions ================================== */

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

static struct PyModuleDef extra_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.extra",
    "Convenience utilities for batch copying operations.",
    -1,
    extra_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

static struct PyModuleDef patch_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.patch",
    "Monkey-patching utilities for stdlib copy module.",
    -1,
    patch_methods,
    NULL,
    NULL,
    NULL,
    NULL
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

/* ===================== Version submodule (__about__) ====================== */

/* Version macros injected at build time */
#ifndef COPIUM_VERSION
    #define COPIUM_VERSION "0.0.0+unknown"
#endif

#ifndef COPIUM_VERSION_MAJOR
    #define COPIUM_VERSION_MAJOR 0
#endif

#ifndef COPIUM_VERSION_MINOR
    #define COPIUM_VERSION_MINOR 0
#endif

#ifndef COPIUM_VERSION_PATCH
    #define COPIUM_VERSION_PATCH 0
#endif

/* COPIUM_VERSION_PRERELEASE is only defined if present (otherwise None) */
/* COPIUM_VERSION_BUILD is only defined if present (otherwise None) */
/* COPIUM_COMMIT_ID is only defined if available (otherwise None) */

/**
 * Create a VersionInfo namedtuple instance from static version macros
 * Expected format:
 *   VersionInfo(
 *       major: int,
 *       minor: int,
 *       patch: int,
 *       prerelease: str | None,
 *       build: int | None,
 *       build_hash: str
 *   )
 */
static PyObject* _create_version_info(PyObject* version_cls) {
#ifndef COPIUM_BUILD_HASH
    #error "COPIUM_BUILD_HASH must be defined by the build backend"
#endif

    /* All objects that need cleanup on error */
    PyObject* major = NULL;
    PyObject* minor = NULL;
    PyObject* patch = NULL;
    PyObject* prerelease = NULL;
    PyObject* build = NULL;
    PyObject* build_hash = NULL;
    PyObject* version_tuple = NULL;

    major = PyLong_FromLong(COPIUM_VERSION_MAJOR);
    if (!major)
        goto error;

    minor = PyLong_FromLong(COPIUM_VERSION_MINOR);
    if (!minor)
        goto error;

    patch = PyLong_FromLong(COPIUM_VERSION_PATCH);
    if (!patch)
        goto error;

    /* Handle prerelease (string or None) */
#ifdef COPIUM_VERSION_PRERELEASE
    prerelease = PyUnicode_FromString(COPIUM_VERSION_PRERELEASE);
    if (!prerelease)
        goto error;
#else
    prerelease = Py_NewRef(Py_None);
#endif

    /* Handle build (int or None) */
#ifdef COPIUM_VERSION_BUILD
    build = PyLong_FromLong(COPIUM_VERSION_BUILD);
    if (!build)
        goto error;
#else
    build = Py_NewRef(Py_None);
#endif

    /* Required build_hash (string) */
    build_hash = PyUnicode_FromString(COPIUM_BUILD_HASH);
    if (!build_hash)
        goto error;

    /* Create VersionInfo instance */
    version_tuple = PyObject_CallFunction(
        version_cls, "OOOOOO", major, minor, patch, prerelease, build, build_hash
    );
    if (!version_tuple)
        goto error;

    /* Success: cleanup temporaries and return */
    Py_DECREF(major);
    Py_DECREF(minor);
    Py_DECREF(patch);
    Py_DECREF(prerelease);
    Py_DECREF(build);
    Py_DECREF(build_hash);
    return version_tuple;

error:
    Py_XDECREF(major);
    Py_XDECREF(minor);
    Py_XDECREF(patch);
    Py_XDECREF(prerelease);
    Py_XDECREF(build);
    Py_XDECREF(build_hash);
    return NULL;
}

static PyModuleDef about_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.__about__",
    "Version information for copium",
    -1,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/* ===================== Helper for adding submodule =========================
 *
 * NOTE: This function does NOT steal or decref the parent reference on failure.
 * The caller is responsible for cleaning up their own references.
 * On success, the submodule reference is stolen by PyModule_AddObject.
 */
static int _add_submodule(PyObject* parent, const char* name, PyObject* submodule) {
    if (!submodule) {
        return -1;
    }

    /* Build full module name (e.g., "copium.patch") */
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

    /* Set __name__ on submodule */
    if (PyObject_SetAttrString(submodule, "__name__", full_name) < 0) {
        Py_DECREF(full_name);
        Py_DECREF(submodule);
        return -1;
    }

    /* Register in sys.modules so it can be imported */
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

    /* Add to parent as attribute */
    if (PyModule_AddObject(parent, name, submodule) < 0) {
        Py_DECREF(submodule);
        return -1;
    }
    /* PyModule_AddObject steals the reference on success */

    return 0;
}

/* ===================== Module initialization =============================== */

PyMODINIT_FUNC PyInit_copium(void) {
#if PY_VERSION_HEX >= 0x03050000
    return PyModuleDef_Init(&main_module_def);
#else
    PyObject* module = PyModule_Create(&main_module_def);
    if (module == NULL)
        return NULL;
    if (copium_exec(module) < 0) {
        Py_DECREF(module);
        return NULL;
    }
    return module;
#endif
}

static int copium_exec(PyObject* module) {
    /* Initialize internal state */
    if (_copium_copying_init(module) < 0) {
        return -1;
    }

    /* Create and attach extra submodule */
    PyObject* extra_module = PyModule_Create(&extra_module_def);
    if (_add_submodule(module, "extra", extra_module) < 0) {
        return -1;
    }

    /* Create and attach patch submodule */
    PyObject* patch_module = PyModule_Create(&patch_module_def);
    if (_add_submodule(module, "patch", patch_module) < 0) {
        return -1;
    }

    /* Add low-level patching API (apply/unapply/applied/get_vectorcall_ptr) to patch module */
    if (_copium_patching_add_api(patch_module) < 0) {
        return -1;
    }

    /* Conditionally create and attach experimental submodule */
    if (_copium_copying_duper_available()) {
        PyObject* experimental_module = PyModule_Create(&experimental_module_def);
        if (_add_submodule(module, "_experimental", experimental_module) < 0) {
            return -1;
        }
    }

    /* === Build __about__ submodule === */

    /* Objects that need cleanup on error in this section */
    PyObject* about_module = NULL;
    PyObject* collections = NULL;
    PyObject* namedtuple = NULL;
    PyObject* version_info_cls = NULL;
    PyObject* version_tuple = NULL;
    PyObject* author_cls = NULL;
    PyObject* author_instance = NULL;
    PyObject* authors_tuple = NULL;

    about_module = PyModule_Create(&about_module_def);
    if (!about_module) {
        return -1;
    }

    /* Add version string to __about__ */
    if (PyModule_AddStringConstant(about_module, "__version__", COPIUM_VERSION) < 0) {
        goto about_error;
    }

    /* Import collections.namedtuple for creating VersionInfo and Author */
    collections = PyImport_ImportModule("collections");
    if (!collections) {
        goto about_error;
    }

    namedtuple = PyObject_GetAttrString(collections, "namedtuple");
    Py_CLEAR(collections);
    if (!namedtuple) {
        goto about_error;
    }

    /* Create VersionInfo namedtuple class */
    version_info_cls = PyObject_CallFunction(
        namedtuple,
        "s[ssssss]",
        "VersionInfo",
        "major",
        "minor",
        "patch",
        "prerelease",
        "build",
        "build_hash"
    );
    if (!version_info_cls) {
        goto about_error;
    }

    /* Add VersionInfo class to __about__ module */
    Py_INCREF(version_info_cls); /* AddObject steals, but we still need it */
    if (PyModule_AddObject(about_module, "VersionInfo", version_info_cls) < 0) {
        Py_DECREF(version_info_cls); /* undo the extra incref */
        goto about_error;
    }

    /* Create VersionInfo instance from static macros */
    version_tuple = _create_version_info(version_info_cls);
    Py_CLEAR(version_info_cls); /* no longer needed */
    if (!version_tuple) {
        goto about_error;
    }

    if (PyModule_AddObject(about_module, "__version_tuple__", version_tuple) < 0) {
        goto about_error;
    }
    version_tuple = NULL; /* stolen by AddObject */

    /* Add __commit_id__ (string or None) */
#ifdef COPIUM_COMMIT_ID
    if (PyModule_AddStringConstant(about_module, "__commit_id__", COPIUM_COMMIT_ID) < 0) {
        goto about_error;
    }
#else
    {
        PyObject* none_ref = Py_NewRef(Py_None);
        if (PyModule_AddObject(about_module, "__commit_id__", none_ref) < 0) {
            Py_DECREF(none_ref);
            goto about_error;
        }
    }
#endif

    /* Add __build_hash__ (required string) */
#ifndef COPIUM_BUILD_HASH
    #error "COPIUM_BUILD_HASH must be defined by the build backend"
#endif
    if (PyModule_AddStringConstant(about_module, "__build_hash__", COPIUM_BUILD_HASH) < 0) {
        goto about_error;
    }

    /* Create Author namedtuple class */
    author_cls = PyObject_CallFunction(namedtuple, "s[ss]", "Author", "name", "email");
    Py_CLEAR(namedtuple);
    if (!author_cls) {
        goto about_error;
    }

    /* Add Author class to __about__ module */
    Py_INCREF(author_cls); /* AddObject steals, but we still need it */
    if (PyModule_AddObject(about_module, "Author", author_cls) < 0) {
        Py_DECREF(author_cls); /* undo the extra incref */
        goto about_error;
    }

    /* Create Author instance */
    author_instance =
        PyObject_CallFunction(author_cls, "ss", "Arseny Boykov (Bobronium)", "hi@bobronium.me");
    Py_CLEAR(author_cls);
    if (!author_instance) {
        goto about_error;
    }

    /* Create tuple containing the author */
    authors_tuple = PyTuple_Pack(1, author_instance);
    Py_CLEAR(author_instance);
    if (!authors_tuple) {
        goto about_error;
    }

    /* Add __authors__ to __about__ module */
    if (PyModule_AddObject(about_module, "__authors__", authors_tuple) < 0) {
        goto about_error;
    }
    authors_tuple = NULL; /* stolen by AddObject */

    /* Attach __about__ to parent module */
    if (_add_submodule(module, "__about__", about_module) < 0) {
        /* _add_submodule already decrefs submodule on failure */
        return -1;
    }

    return 0;

about_error:
    Py_XDECREF(about_module);
    Py_XDECREF(collections);
    Py_XDECREF(namedtuple);
    Py_XDECREF(version_info_cls);
    Py_XDECREF(version_tuple);
    Py_XDECREF(author_cls);
    Py_XDECREF(author_instance);
    Py_XDECREF(authors_tuple);
    return -1;
}