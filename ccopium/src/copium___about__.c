/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium.__about__ submodule
 *
 * Version and authorship information:
 *   - __version__       : str
 *   - __version_tuple__ : VersionInfo namedtuple
 *   - __commit_id__     : str | None
 *   - __build_hash__    : str
 *   - __authors__       : tuple[Author, ...]
 *   - VersionInfo       : namedtuple class
 *   - Author            : namedtuple class
 */
#ifndef COPIUM___ABOUT___C
#define COPIUM___ABOUT___C

#include "_common.h"

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

#ifndef COPIUM_BUILD_HASH
    #error "COPIUM_BUILD_HASH must be defined by the build backend"
#endif

/* ------------------------------------------------------------------------- */

/**
 * Create a VersionInfo namedtuple instance from static version macros.
 *
 * VersionInfo(major, minor, patch, pre, dev, local)
 */
static PyObject* _create_version_info(PyObject* version_cls) {
    PyObject* major = NULL;
    PyObject* minor = NULL;
    PyObject* patch = NULL;
    PyObject* pre = NULL;
    PyObject* dev = NULL;
    PyObject* local = NULL;
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

#ifdef COPIUM_VERSION_PRE
    pre = PyUnicode_FromString(COPIUM_VERSION_PRE);
    if (!pre)
        goto error;
#else
    pre = Py_NewRef(Py_None);
#endif

#ifdef COPIUM_VERSION_DEV
    dev = PyLong_FromLong(COPIUM_VERSION_DEV);
    if (!dev)
        goto error;
#else
    dev = Py_NewRef(Py_None);
#endif

    local = PyUnicode_FromString(COPIUM_BUILD_HASH);
    if (!local)
        goto error;

    version_tuple = PyObject_CallFunction(
        version_cls, "OOOOOO", major, minor, patch, pre, dev, local
    );
    if (!version_tuple)
        goto error;

    Py_DECREF(major);
    Py_DECREF(minor);
    Py_DECREF(patch);
    Py_DECREF(pre);
    Py_DECREF(dev);
    Py_DECREF(local);
    return version_tuple;

error:
    Py_XDECREF(major);
    Py_XDECREF(minor);
    Py_XDECREF(patch);
    Py_XDECREF(pre);
    Py_XDECREF(dev);
    Py_XDECREF(local);
    return NULL;
}

/* ------------------------------------------------------------------------- */

static struct PyModuleDef about_module_def = {
    PyModuleDef_HEAD_INIT,
    "copium.__about__",
    "Version information for copium.",
    -1,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/**
 * Build and attach the __about__ submodule to the parent module.
 *
 * Returns 0 on success, -1 on error.
 */
static int _build_about_module(
    PyObject* parent, int (*add_submodule)(PyObject*, const char*, PyObject*)
) {
    PyObject* about_module = NULL;
    PyObject* collections = NULL;
    PyObject* namedtuple = NULL;
    PyObject* version_info_cls = NULL;
    PyObject* version_tuple = NULL;
    PyObject* author_cls = NULL;
    PyObject* author_instance = NULL;
    PyObject* authors_tuple = NULL;

    about_module = PyModule_Create(&about_module_def);
    if (!about_module)
        return -1;

    /* __version__ */
    if (PyModule_AddStringConstant(about_module, "__version__", COPIUM_VERSION) < 0)
        goto error;

    /* Import namedtuple */
    collections = PyImport_ImportModule("collections");
    if (!collections)
        goto error;

    namedtuple = PyObject_GetAttrString(collections, "namedtuple");
    Py_CLEAR(collections);
    if (!namedtuple)
        goto error;

    /* VersionInfo class */
    version_info_cls = PyObject_CallFunction(
        namedtuple, "s[ssssss]", "VersionInfo", "major", "minor", "patch", "pre", "dev", "local"
    );
    if (!version_info_cls)
        goto error;

    Py_INCREF(version_info_cls);
    if (PyModule_AddObject(about_module, "VersionInfo", version_info_cls) < 0) {
        Py_DECREF(version_info_cls);
        goto error;
    }

    /* __version_tuple__ */
    version_tuple = _create_version_info(version_info_cls);
    Py_CLEAR(version_info_cls);
    if (!version_tuple)
        goto error;

    if (PyModule_AddObject(about_module, "__version_tuple__", version_tuple) < 0)
        goto error;
    version_tuple = NULL;

    /* __commit_id__ */
#ifdef COPIUM_COMMIT_ID
    if (PyModule_AddStringConstant(about_module, "__commit_id__", COPIUM_COMMIT_ID) < 0)
        goto error;
#else
    {
        PyObject* none_ref = Py_NewRef(Py_None);
        if (PyModule_AddObject(about_module, "__commit_id__", none_ref) < 0) {
            Py_DECREF(none_ref);
            goto error;
        }
    }
#endif

    /* __build_hash__ */
    if (PyModule_AddStringConstant(about_module, "__build_hash__", COPIUM_BUILD_HASH) < 0)
        goto error;

    /* Author class */
    author_cls = PyObject_CallFunction(namedtuple, "s[ss]", "Author", "name", "email");
    Py_CLEAR(namedtuple);
    if (!author_cls)
        goto error;

    Py_INCREF(author_cls);
    if (PyModule_AddObject(about_module, "Author", author_cls) < 0) {
        Py_DECREF(author_cls);
        goto error;
    }

    /* Author instance */
    author_instance = PyObject_CallFunction(
        author_cls, "ss", "Arseny Boykov (Bobronium)", "hi@bobronium.me"
    );
    Py_CLEAR(author_cls);
    if (!author_instance)
        goto error;

    /* __authors__ */
    authors_tuple = PyTuple_Pack(1, author_instance);
    Py_CLEAR(author_instance);
    if (!authors_tuple)
        goto error;

    if (PyModule_AddObject(about_module, "__authors__", authors_tuple) < 0)
        goto error;
    authors_tuple = NULL;

    /* Attach to parent */
    if (add_submodule(parent, "__about__", about_module) < 0)
        return -1;

    return 0;

error:
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

#endif /* COPIUM___ABOUT___C */