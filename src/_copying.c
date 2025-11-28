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

#include <stddef.h> /* ptrdiff_t */
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
#include "pycore_object.h" /* _PyNone_Type, _PyNotImplemented_Type */

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

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
static Py_tss_t g_dictiter_tss = Py_tss_NEEDS_INIT;
static int g_dict_watcher_id = -1;
static int g_dict_watcher_registered = 0;
#endif

#if PY_VERSION_HEX < PY_VERSION_3_13_HEX
    #define PyObject_GetOptionalAttr(obj, name, out) _PyObject_LookupAttr((obj), (name), (out))
#endif

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX

typedef struct DictIterGuard {
    PyObject* dict;
    Py_ssize_t pos;
    Py_ssize_t size0; /* initial size snapshot (detect transient size changes like pop→add) */
    int watching;
    int mutated;
    int size_changed; /* set if size ever differed from size0; else keys-only changes */
    int last_event;   /* PyDict_WatchEvent (for debugging/future use) */
    struct DictIterGuard* prev;
} DictIterGuard;

/* Walk the TLS stack and flag the top-most guard matching `dict`. */
static int _copium_dict_watcher_cb(
    PyDict_WatchEvent event, PyObject* dict, PyObject* key, PyObject* new_value
) {
    (void)key;
    (void)new_value;
    DictIterGuard* g = (DictIterGuard*)PyThread_tss_get(&g_dictiter_tss);
    for (; g; g = g->prev) {
        if (g->dict == dict) {
            g->mutated = 1;
            g->last_event = (int)event;

            /* Treat as "changed size" if:
               - the event is an add/delete/clear, OR
               - the dict's current size differs from the initial snapshot (transient del→add). */
            if (event == PyDict_EVENT_ADDED || event == PyDict_EVENT_DELETED ||
                event == PyDict_EVENT_CLEARED || event == PyDict_EVENT_CLONED) {
                g->size_changed = 1;
            } else {
                Py_ssize_t cur = PyDict_Size(dict);
                if (cur >= 0 && cur != g->size0) {
                    g->size_changed = 1;
                }
            }
            /* Other events (CLONED, MODIFIED, etc.) fall back to "keys changed". */
            /* DO NOT break: mark all active guards for this dict (handles nested iterations). */
        }
    }
    return 0; /* never raise; per docs this would become unraisable */
}

static ALWAYS_INLINE void dict_iter_init(DictIterGuard* iter_guard, PyObject* dict) {
    iter_guard->dict = dict;
    iter_guard->pos = 0;
    iter_guard->size0 =
        PyDict_Size(dict); /* snapshot initial size (errors unlikely; -1 harmless) */
    iter_guard->watching = 0;
    iter_guard->mutated = 0;
    iter_guard->size_changed = 0;
    iter_guard->last_event = 0;
    iter_guard->prev = (DictIterGuard*)PyThread_tss_get(&g_dictiter_tss);

    /* Push onto TLS stack first so any same-thread mutation after Watch sees this guard. */
    if (PyThread_tss_set(&g_dictiter_tss, iter_guard) != 0) {
        Py_FatalError("copium: unexpected TTS state - failed to set dict iterator guard");
    }

    if (g_dict_watcher_registered && PyDict_Watch(g_dict_watcher_id, dict) == 0) {
        iter_guard->watching = 1;
    }
}

static ALWAYS_INLINE void dict_iter_cleanup(DictIterGuard* iter_guard) {
    /* Unwatch first, before clearing dict pointer */
    if (iter_guard->watching) {
        PyDict_Unwatch(g_dict_watcher_id, iter_guard->dict);
        iter_guard->watching = 0;
    }

    /* Pop from TLS stack - we're at the top in normal cases */
    DictIterGuard* top = (DictIterGuard*)PyThread_tss_get(&g_dictiter_tss);
    if (top == iter_guard) {
        /* Normal case: we're on top of the stack */
        if (PyThread_tss_set(&g_dictiter_tss, iter_guard->prev) != 0) {
            Py_FatalError("copium: unexpected TTS state during dict iterator cleanup");
        }
    }
    /* Note: If we're not on top (defensive unlink case), we just skip TSS cleanup.
       The stack will correct itself when the actual top guard is cleaned up. */
}

static ALWAYS_INLINE int dict_iter_next(
    DictIterGuard* iter_guard, PyObject** key, PyObject** value
) {
    if (PyDict_Next(iter_guard->dict, &iter_guard->pos, key, value)) {
        if (UNLIKELY(iter_guard->mutated)) {
            /* Decide message based on net size delta from start of iteration. */
            int size_changed_now = 0;
            Py_ssize_t cur = PyDict_Size(iter_guard->dict);
            if (cur >= 0) {
                size_changed_now = (cur != iter_guard->size0);
            } else {
                /* Fallback if PyDict_Size errored: use watcher heuristic. */
                size_changed_now = iter_guard->size_changed;
            }
            PyErr_SetString(
                PyExc_RuntimeError,
                size_changed_now ? "dictionary changed size during iteration"
                                 : "dictionary keys changed during iteration"
            );
            dict_iter_cleanup(iter_guard);
            return -1;
        }
        return 1;
    }
    /* End of iteration. If a mutation happened at any point, surface it now. */
    if (UNLIKELY(iter_guard->mutated)) {
        int size_changed_now = 0;
        Py_ssize_t cur = PyDict_Size(iter_guard->dict);
        if (cur >= 0) {
            size_changed_now = (cur != iter_guard->size0);
        } else {
            size_changed_now = iter_guard->size_changed;
        }
        PyErr_SetString(
            PyExc_RuntimeError,
            size_changed_now ? "dictionary changed size during iteration"
                             : "dictionary keys changed during iteration"
        );
        dict_iter_cleanup(iter_guard);
        return -1;
    }
    dict_iter_cleanup(iter_guard);
    return 0;
}

#else /* < 3.14: keep version-tag based guard (uses private fields, but gated) */

typedef struct {
    PyObject* dict;
    Py_ssize_t pos;
    uint64_t ver0;
    Py_ssize_t used0;
} DictIterGuard;

static ALWAYS_INLINE void dict_iter_init(DictIterGuard* iter_guard, PyObject* dict) {
    iter_guard->dict = dict;
    iter_guard->pos = 0;
    iter_guard->ver0 = ((PyDictObject*)dict)->ma_version_tag;
    iter_guard->used0 = ((PyDictObject*)dict)->ma_used;
}

static ALWAYS_INLINE int dict_iter_next(
    DictIterGuard* iter_guard, PyObject** key, PyObject** value
) {
    if (PyDict_Next(iter_guard->dict, &iter_guard->pos, key, value)) {
        uint64_t ver_now = ((PyDictObject*)iter_guard->dict)->ma_version_tag;
        if (UNLIKELY(ver_now != iter_guard->ver0)) {
            Py_ssize_t used_now = ((PyDictObject*)iter_guard->dict)->ma_used;
            if (used_now != iter_guard->used0) {
                PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
            } else {
                PyErr_SetString(PyExc_RuntimeError, "dictionary keys changed during iteration");
            }
            return -1;
        }
        return 1;
    }
    return 0;
}
#endif


/* ------------------------------ Atomic checks ------------------------------
 */

static ALWAYS_INLINE int is_literal_immutable(PyTypeObject* tp) {
    // First tier: the most popular literal immutables.
    unsigned long r = (tp == &_PyNone_Type) | (tp == &PyLong_Type) | (tp == &PyUnicode_Type) |
        (tp == &PyBool_Type) | (tp == &PyFloat_Type) | (tp == &PyBytes_Type);
    return (int)r;
}

static ALWAYS_INLINE int is_builtin_immutable(PyTypeObject* tp) {
    // Second tier: less common than builtin containers but still builtin immutables.
    unsigned long r = (tp == &PyRange_Type) | (tp == &PyFunction_Type) | (tp == &PyCFunction_Type) |
        (tp == &PyProperty_Type) | (tp == &_PyWeakref_RefType) | (tp == &PyCode_Type) |
        (tp == &PyModule_Type) | (tp == &_PyNotImplemented_Type) | (tp == &PyEllipsis_Type) |
        (tp == &PyComplex_Type);
    return (int)r;
}

static ALWAYS_INLINE int is_stdlib_immutable(PyTypeObject* tp) {
    // Third tier: stdlib immutables cached at runtime (requires module_state).
    unsigned long r = (tp == module_state.re_Pattern_type) | (tp == module_state.Decimal_type) |
        (tp == module_state.Fraction_type);
    return (int)r;
}

static ALWAYS_INLINE int is_class(PyTypeObject* tp) {
    // Type objects themselves and type-subclasses are immutable.
    return PyType_HasFeature(tp, Py_TPFLAGS_TYPE_SUBCLASS);
}

static ALWAYS_INLINE int is_atomic_immutable(PyTypeObject* tp) {
    // Consolidated type-based predicate (no object needed).
    unsigned long r = (unsigned long)is_literal_immutable(tp) |
        (unsigned long)is_builtin_immutable(tp) | (unsigned long)is_class(tp) |
        (unsigned long)is_stdlib_immutable(tp);
    return (int)r;
}

/* ------------------------ TLS memo & recursion guard ------------------------
 */

static ALWAYS_INLINE PyObject* get_tss_memo(void) {
    void* val = PyThread_tss_get(&module_state.memo_tss);
    if (val == NULL) {
        PyObject* memo = Memo_New();
        if (memo == NULL)
            return NULL;
        if (PyThread_tss_set(&module_state.memo_tss, (void*)memo) != 0) {
            Py_DECREF(memo);
            Py_FatalError("copium: unexpected TTS state - failed to set memo");
        }
        return memo;
    }

    PyObject* existing = (PyObject*)val;
    if (Py_REFCNT(existing) > 1) {
        // Memo got stolen in between runs somehow.
        // Highly unlikely, but we'll detach it anyway and enable gc tracking for it.
        PyObject_GC_Track(existing);

        PyObject* memo = Memo_New();
        if (memo == NULL)
            return NULL;

        if (PyThread_tss_set(&module_state.memo_tss, (void*)memo) != 0) {
            Py_DECREF(memo);
            Py_FatalError("copium: unexpected TTS state - failed to replace memo");
        }
        return memo;
    }

    return existing;
}

static ALWAYS_INLINE int cleanup_tss_memo(MemoObject* memo, PyObject* memo_local) {
    Py_ssize_t refcount = Py_REFCNT(memo_local);

    if (refcount == 1) {
        keepalive_clear(&memo->keepalive);
        keepalive_shrink_if_large(&memo->keepalive);
        memo_table_reset(&memo->table);
        return 1;
    } else {
        PyObject_GC_Track(memo_local);
        if (PyThread_tss_set(&module_state.memo_tss, NULL) != 0) {
            Py_DECREF(memo_local);
            Py_FatalError("copium: unexpected TTS state during memo cleanup");
        }
        Py_DECREF(memo_local);
        return 0;
    }
}

/* ------------------------- Recursion depth guard (stack cap) ----------------
 * (unchanged: we only sample when entering fallback/reconstructor)
 */

#ifndef COPIUM_STACKCHECK_STRIDE
    #define COPIUM_STACKCHECK_STRIDE 32u
#endif

#ifndef COPIUM_STACK_SAFETY_MARGIN
    #define COPIUM_STACK_SAFETY_MARGIN (256u * 1024u)  // 256 KiB
#endif

/* ------------------------- Thread-local storage ----------------------------- */
#ifdef _MSC_VER
    #define COPIUM_THREAD_LOCAL __declspec(thread)
#else
    #define COPIUM_THREAD_LOCAL _Thread_local
#endif

/* ------------------------- Stack overflow protection ------------------------ */
static COPIUM_THREAD_LOCAL unsigned int _copium_tls_depth = 0;
static COPIUM_THREAD_LOCAL char* _copium_stack_low = NULL;
static COPIUM_THREAD_LOCAL int _copium_stack_inited = 0;

static void _copium_init_stack_bounds(void) {
    _copium_stack_inited = 1;

#if defined(__APPLE__)
    pthread_t t = pthread_self();
    size_t sz = pthread_get_stacksize_np(t);
    void* base = pthread_get_stackaddr_np(t);
    char* high = (char*)base;
    char* low = high - (ptrdiff_t)sz;
    if (sz > COPIUM_STACK_SAFETY_MARGIN)
        low += COPIUM_STACK_SAFETY_MARGIN;
    _copium_stack_low = low;

#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = NULL;
        size_t sz = 0;
        if (pthread_attr_getstack(&attr, &addr, &sz) == 0 && addr && sz) {
            char* low = (char*)addr;
            if (sz > COPIUM_STACK_SAFETY_MARGIN)
                low += COPIUM_STACK_SAFETY_MARGIN;
            _copium_stack_low = low;
        }
        pthread_attr_destroy(&attr);
    }

#elif defined(_WIN32)
    typedef VOID(WINAPI * GetStackLimitsFn)(PULONG_PTR, PULONG_PTR);
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        GetStackLimitsFn fn =
            (GetStackLimitsFn)GetProcAddress(hKernel32, "GetCurrentThreadStackLimits");
        if (fn) {
            ULONG_PTR low = 0, high = 0;
            fn(&low, &high);
            size_t sz = (size_t)(high - low);
            char* lowc = (char*)low;
            if (sz > COPIUM_STACK_SAFETY_MARGIN)
                lowc += COPIUM_STACK_SAFETY_MARGIN;
            _copium_stack_low = lowc;
        }
    }
#endif
}

static ALWAYS_INLINE int _copium_recursion_enter(void) {
    unsigned int d = ++_copium_tls_depth;

    if (LIKELY(d < COPIUM_STACKCHECK_STRIDE)) {
        return 0;
    }

    if ((d & (COPIUM_STACKCHECK_STRIDE - 1u)) == 0u) {
        if (UNLIKELY(!_copium_stack_inited)) {
            _copium_init_stack_bounds();
        }

        if (_copium_stack_low) {
            char sp_probe;
            char* sp = (char*)&sp_probe;
            if (UNLIKELY(sp <= _copium_stack_low)) {
                _copium_tls_depth--;
                PyErr_Format(
                    PyExc_RecursionError,
                    "Stack overflow (depth %u) while deep copying an object",
                    d
                );
                return -1;
            }
        } else {
            // Not Windows/Linux/macOS, this technically might lead to crash
            // if recursion limit is set to unreasonably high value.
            // But case is esoteric enough to ignore it for now.
            int limit = Py_GetRecursionLimit();
            if (UNLIKELY((int)d > limit)) {
                _copium_tls_depth--;
                PyErr_Format(
                    PyExc_RecursionError,
                    "Stack overflow (depth %u) while deep copying an object",
                    d
                );
                return -1;
            }
        }
    }
    return 0;
}

static ALWAYS_INLINE void _copium_recursion_leave(void) {
    if (_copium_tls_depth > 0)
        _copium_tls_depth--;
}

#define RECURSION_GUARDED(expr)                        \
    (__extension__({                                   \
        PyObject* _ret;                                \
        if (UNLIKELY(_copium_recursion_enter() < 0)) { \
            _ret = NULL;                               \
        } else {                                       \
            _ret = (expr);                             \
            _copium_recursion_leave();                 \
        }                                              \
        _ret;                                          \
    }))
/* ----------------------- Python-dict memo helpers (inline) ------------------ */

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
            memo, module_state.str_get, pykey, module_state.sentinel, NULL
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

/* ----------------------------- Predecl for c/py paths ---------------------- */

static ALWAYS_INLINE PyObject* deepcopy(PyObject* obj, MemoObject* memo);
static ALWAYS_INLINE PyObject* deepcopy_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer
);

/* ----------------------------- Type-special helpers ------------------------ */
/* We define two sets of helpers: *_c operate with MemoObject*, *_py with dict. */
/* Each set inlines dispatch and only recurses after type resolution.          */

/* === C-memo specializations ================================================= */

// Keep-append helper for C-memo path.
// Returns 0 on success, -1 on failure. Caller handles cleanup.
static ALWAYS_INLINE int maybe_keepalive(PyObject* copy, PyObject* src, MemoObject* memo) {
    if (copy != src) {
        return keepalive_append(&memo->keepalive, src);
    }
    return 0;
}

static MAYBE_INLINE PyObject* deepcopy_list(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_tuple(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_dict(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_set(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static MAYBE_INLINE PyObject* deepcopy_frozenset(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_bytearray(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_method(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash);
static PyObject* deepcopy_object(
    PyObject* obj, PyTypeObject* tp, MemoObject* memo, Py_ssize_t id_hash
);

static ALWAYS_INLINE PyObject* deepcopy(PyObject* obj, MemoObject* memo) {
    assert(memo != NULL && "deepcopy_c: memo must not be NULL");

    PyTypeObject* tp = Py_TYPE(obj);
    /* 1) Immortal or literal immutables → fastest return */
    if (LIKELY(is_literal_immutable(tp))) {
        return Py_NewRef(obj);
    }

    /* 2) Memo hit */
    void* id = (void*)obj;
    Py_ssize_t h = memo_hash_pointer(id);
    PyObject* hit = memo_table_lookup_h(memo->table, id, h);
    if (hit)
        return Py_NewRef(hit);

    /* 3) Popular containers first (specialized, likely hot) */
    if (tp == &PyDict_Type)
        return RECURSION_GUARDED(deepcopy_dict(obj, memo, h));
    if (tp == &PyList_Type)
        return RECURSION_GUARDED(deepcopy_list(obj, memo, h));
    if (tp == &PyTuple_Type)
        return RECURSION_GUARDED(deepcopy_tuple(obj, memo, h));
    if (tp == &PySet_Type)
        return RECURSION_GUARDED(deepcopy_set(obj, memo, h));

    /* 4) Other atomic immutables (builtin/class types) */
    if (is_builtin_immutable(tp) || is_class(tp)) {
        return Py_NewRef(obj);
    }

    if (tp == &PyFrozenSet_Type)
        return deepcopy_frozenset(obj, memo, h);

    if (tp == &PyByteArray_Type)
        return deepcopy_bytearray(obj, memo, h);

    if (tp == &PyMethod_Type)
        return deepcopy_method(obj, memo, h);

    if (is_stdlib_immutable(tp))  // touch non-static types last
        return Py_NewRef(obj);

    /* Robustly detect a user-defined __deepcopy__ via optional lookup (single step, non-raising on miss). */
    {
        PyObject* deepcopy_meth = NULL;
        int has_deepcopy = PyObject_GetOptionalAttr(obj, module_state.str_deepcopy, &deepcopy_meth);
        if (has_deepcopy < 0)
            return NULL;
        if (has_deepcopy) {
            PyObject* res = PyObject_CallOneArg(deepcopy_meth, (PyObject*)memo);
            Py_DECREF(deepcopy_meth);
            if (!res)
                return NULL;
            if (res != obj) {
                if (memo_table_insert_h(&memo->table, (void*)obj, res, h) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
                if (keepalive_append(&memo->keepalive, obj) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
            }
            return res;
        }
    }

    PyObject* res = deepcopy_object(obj, tp, memo, h);
    return res;
}

static MAYBE_INLINE PyObject* deepcopy_list(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* copied_item = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = PyList_New(sz);
    if (!copy)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyList_SET_ITEM(copy, i, Py_NewRef(Py_None));
    }
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyList_GET_ITEM(obj, i);
        copied_item = deepcopy(item, memo);
        if (!copied_item)
            goto error;
        Py_DECREF(Py_None);
        PyList_SET_ITEM(copy, i, copied_item);
        copied_item = NULL;  // ownership transferred to list
    }

    /* If the source list grew during the first pass, append the extra tail items. */
    Py_ssize_t i2 = sz;
    while (i2 < Py_SIZE(obj)) {
        PyObject* item2 = PyList_GET_ITEM(obj, i2);
        copied_item = deepcopy(item2, memo);
        if (!copied_item)
            goto error;
        if (PyList_Append(copy, copied_item) < 0)
            goto error;
        Py_DECREF(copied_item);
        copied_item = NULL;
        i2++;
    }
    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_tuple(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* copied = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = PyTuple_New(sz);
    if (!copy)
        goto error;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyTuple_GET_ITEM(obj, i);
        copied = deepcopy(item, memo);
        if (!copied)
            goto error;
        if (copied != item)
            all_same = 0;
        PyTuple_SET_ITEM(copy, i, copied);
        copied = NULL;  // ownership transferred to tuple
    }
    if (all_same) {
        Py_DECREF(copy);
        return Py_NewRef(obj);
    }

    /* Handle self-referential tuples: if a recursive path already created a copy,
       prefer that existing copy to maintain identity. */
    PyObject* existing = memo_table_lookup_h(memo->table, (void*)obj, id_hash);
    if (existing) {
        Py_DECREF(copy);
        return Py_NewRef(existing);
    }

    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_dict(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* ckey = NULL;
    PyObject* cvalue = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = _PyDict_NewPresized(sz);
    if (!copy)
        goto error_no_cleanup;
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error_no_cleanup;

    // NOTE: iter_guard is declared here, after early returns.
    // error_no_cleanup is for paths before iter_guard is initialized or after dict_iter_next
    // already cleaned up. error is for mid-iteration cleanup (3.14+ only).
    DictIterGuard iter_guard;
    dict_iter_init(&iter_guard, obj);

    PyObject *key, *value;
    int ret;
    while ((ret = dict_iter_next(&iter_guard, &key, &value)) > 0) {
        ckey = deepcopy(key, memo);
        if (!ckey)
            goto error;
        cvalue = deepcopy(value, memo);
        if (!cvalue)
            goto error;
        if (PyDict_SetItem(copy, ckey, cvalue) < 0)
            goto error;
        Py_DECREF(ckey);
        ckey = NULL;
        Py_DECREF(cvalue);
        cvalue = NULL;
    }
    if (ret < 0)
        goto error_no_cleanup;  // dict_iter_next already cleaned up on -1

    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    dict_iter_cleanup(&iter_guard);
#endif
error_no_cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(ckey);
    Py_XDECREF(cvalue);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_set(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* snap = NULL;
    PyObject* citem = NULL;

    copy = PySet_New(NULL);
    if (!copy)
        goto error;
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;

    /* Snapshot into a pre-sized tuple without invoking user code. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        goto error;
    snap = PyTuple_New(n);
    if (!snap)
        goto error;

    Py_ssize_t pos = 0;
    PyObject* item;
    Py_hash_t hash;
    Py_ssize_t i = 0;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        if (i < n) {
            /* item is borrowed; store owned ref in the tuple */
            Py_INCREF(item);
            PyTuple_SET_ITEM(snap, i, item);
            i++;
        } else {
            /* If the set grows during snapshotting, ignore extras to avoid overflow. */
        }
    }

    for (Py_ssize_t j = 0; j < i; j++) {
        PyObject* elem = PyTuple_GET_ITEM(snap, j); /* borrowed from snapshot */
        citem = deepcopy(elem, memo);
        if (!citem)
            goto error;
        if (PySet_Add(copy, citem) < 0)
            goto error;
        Py_DECREF(citem);
        citem = NULL;
    }
    Py_DECREF(snap);

    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(snap);
    Py_XDECREF(citem);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* temp = NULL;
    PyObject* copy = NULL;
    PyObject* citem = NULL;

    /* Pre-size snapshot: frozenset is immutable, so size won't change mid-loop. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        goto error;

    temp = PyTuple_New(n);
    if (!temp)
        goto error;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        /* item is borrowed; deepcopy_c returns a new reference which tuple will own. */
        citem = deepcopy(item, memo);
        if (!citem)
            goto error;
        PyTuple_SET_ITEM(temp, i, citem);  // steals reference to citem
        citem = NULL;                      // ownership transferred
        i++;
    }

    copy = PyFrozenSet_New(temp);
    Py_DECREF(temp);
    temp = NULL;
    if (!copy)
        goto error;
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(temp);
    Py_XDECREF(copy);
    Py_XDECREF(citem);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_bytearray(
    PyObject* obj, MemoObject* memo, Py_ssize_t id_hash
) {
    PyObject* copy = NULL;

    Py_ssize_t sz = PyByteArray_Size(obj);
    copy = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copy)
        goto error;
    if (sz)
        memcpy(PyByteArray_AS_STRING(copy), PyByteArray_AS_STRING(obj), (size_t)sz);
    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(copy);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_method(PyObject* obj, MemoObject* memo, Py_ssize_t id_hash) {
    // Owned refs that need cleanup on error
    PyObject* func = NULL;
    PyObject* self = NULL;
    PyObject* cself = NULL;
    PyObject* copy = NULL;

    func = PyMethod_GET_FUNCTION(obj);
    self = PyMethod_GET_SELF(obj);
    if (!func || !self)
        goto error;

    Py_INCREF(func);
    Py_INCREF(self);
    cself = deepcopy(self, memo);
    Py_DECREF(self);
    self = NULL;
    if (!cself)
        goto error;

    copy = PyMethod_New(func, cself);
    Py_DECREF(func);
    func = NULL;
    Py_DECREF(cself);
    cself = NULL;
    if (!copy)
        goto error;

    if (memo_table_insert_h(&memo->table, (void*)obj, copy, id_hash) < 0)
        goto error;
    if (maybe_keepalive(copy, obj, memo) < 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return copy;

error:
    Py_XDECREF(func);
    Py_XDECREF(self);
    Py_XDECREF(cself);
    Py_XDECREF(copy);
    return NULL;
}

// Try to find a reducer in copyreg.dispatch_table for the given type.
//
// Returns:
// - New reference to reduce result on success (reducer found and called)
// - NULL with PyErr_Occurred() on error (reducer failed)
// - NULL without error if type not in dispatch table (caller should try __reduce_ex__)
//
static PyObject* try_reduce_via_registry(PyObject* obj, PyTypeObject* tp) {
    PyObject* reducer = PyDict_GetItemWithError(module_state.copyreg_dispatch, (PyObject*)tp);
    if (!reducer) {
        // NULL can mean "not found" or "error during lookup"
        if (PyErr_Occurred())
            return NULL;
        return NULL;  // not found, no error - caller should try __reduce_ex__
    }
    if (!PyCallable_Check(reducer)) {
        PyErr_SetString(PyExc_TypeError, "copyreg.dispatch_table value is not callable");
        return NULL;
    }
    return PyObject_CallOneArg(reducer, obj);
}

static PyObject* call_reduce_method_preferring_ex(PyObject* obj) {
    PyObject* reduce_ex = NULL;
    int has_reduce_ex = PyObject_GetOptionalAttr(obj, module_state.str_reduce_ex, &reduce_ex);
    if (has_reduce_ex > 0) {
        PyObject* res = PyObject_CallFunction(reduce_ex, "i", 4);
        Py_DECREF(reduce_ex);
        return res;
    }
    if (has_reduce_ex < 0)
        return NULL;

    PyObject* reduce = NULL;
    int has_reduce = PyObject_GetOptionalAttr(obj, module_state.str_reduce, &reduce);
    if (has_reduce > 0) {
        PyObject* res = PyObject_CallNoArgs(reduce);
        Py_DECREF(reduce);
        return res;
    }
    if (has_reduce < 0)
        return NULL;

    PyErr_SetString(
        (PyObject*)module_state.copy_Error, "un(deep)copyable object (no reduce protocol)"
    );
    return NULL;
}

// Return values for validate_reduce_tuple:
// REDUCE_ERROR  (-1): Invalid reduce result, exception set
// REDUCE_TUPLE  (0):  Valid tuple, out params filled
// REDUCE_STRING (1):  String/bytes shortcut, out params NULL'd, caller returns obj as-is
//
typedef enum {
    REDUCE_ERROR = -1,
    REDUCE_TUPLE = 0,
    REDUCE_STRING = 1
} ReduceValidation;

static int validate_reduce_tuple(
    PyObject* reduce_result,
    PyObject** out_callable,
    PyObject** out_argtup,
    PyObject** out_state,
    PyObject** out_listitems,
    PyObject** out_dictitems
) {
    if (!PyTuple_Check(reduce_result)) {
        if (PyUnicode_Check(reduce_result) || PyBytes_Check(reduce_result)) {
            *out_callable = *out_argtup = *out_state = *out_listitems = *out_dictitems = NULL;
            return REDUCE_STRING;
        }
        PyErr_SetString(PyExc_TypeError, "__reduce__ must return a tuple or str");
        return REDUCE_ERROR;
    }

    Py_ssize_t size = PyTuple_GET_SIZE(reduce_result);
    if (size < 2 || size > 5) {
        PyErr_SetString(
            PyExc_TypeError, "tuple returned by __reduce__ must contain 2 through 5 elements"
        );
        return REDUCE_ERROR;
    }

    PyObject* callable = PyTuple_GET_ITEM(reduce_result, 0);
    PyObject* argtup = PyTuple_GET_ITEM(reduce_result, 1);
    PyObject* state = (size >= 3) ? PyTuple_GET_ITEM(reduce_result, 2) : Py_None;
    PyObject* listitems = (size >= 4) ? PyTuple_GET_ITEM(reduce_result, 3) : Py_None;
    PyObject* dictitems = (size == 5) ? PyTuple_GET_ITEM(reduce_result, 4) : Py_None;

    if (!PyCallable_Check(callable)) {
        PyErr_Format(
            PyExc_TypeError,
            "first item of the tuple returned by __reduce__ must be callable, not %.200s",
            Py_TYPE(callable)->tp_name
        );
        return REDUCE_ERROR;
    }

    if (!PyTuple_Check(argtup)) {
        PyErr_Format(
            PyExc_TypeError,
            "second item of the tuple returned by __reduce__ must be a tuple, not %.200s",
            Py_TYPE(argtup)->tp_name
        );
        return REDUCE_ERROR;
    }

    if (listitems != Py_None && !PyIter_Check(listitems)) {
        PyErr_Format(
            PyExc_TypeError,
            "fourth item of the tuple returned by __reduce__ must be an iterator, not %.200s",
            Py_TYPE(listitems)->tp_name
        );
        return REDUCE_ERROR;
    }

    if (dictitems != Py_None && !PyIter_Check(dictitems)) {
        PyErr_Format(
            PyExc_TypeError,
            "fifth item of the tuple returned by __reduce__ must be an iterator, not %.200s",
            Py_TYPE(dictitems)->tp_name
        );
        return REDUCE_ERROR;
    }

    *out_callable = callable;
    *out_argtup = argtup;
    *out_state = (state == Py_None) ? NULL : state;
    *out_listitems = (listitems == Py_None) ? NULL : listitems;
    *out_dictitems = (dictitems == Py_None) ? NULL : dictitems;

    return REDUCE_TUPLE;
}

// Reduce protocol implementation for C-memo path.
// This is the cold fallback path - not marked ALWAYS_INLINE to avoid code bloat.
static PyObject* deepcopy_object(
    PyObject* obj, PyTypeObject* tp, MemoObject* memo, Py_ssize_t id_hash
) {
    // All owned refs declared at top for cleanup
    PyObject* reduce_result = NULL;
    PyObject* inst = NULL;
    PyObject* setstate = NULL;
    PyObject* state_copy = NULL;
    PyObject* result = NULL;
    PyObject* dict_state_copy = NULL;
    PyObject* inst_dict = NULL;
    PyObject* slotstate_copy = NULL;
    PyObject* append = NULL;
    PyObject* it = NULL;
    PyObject* item_copy = NULL;
    PyObject* key_copy = NULL;
    PyObject* value_copy = NULL;

    reduce_result = try_reduce_via_registry(obj, tp);
    if (!reduce_result) {
        if (PyErr_Occurred())
            goto error;
        reduce_result = call_reduce_method_preferring_ex(obj);
        if (!reduce_result)
            goto error;
    }

    PyObject *callable, *argtup, *state, *listitems, *dictitems;
    int valid =
        validate_reduce_tuple(reduce_result, &callable, &argtup, &state, &listitems, &dictitems);
    if (valid == REDUCE_ERROR)
        goto error;
    if (valid == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(obj);
    }

    // Handle __newobj__
    if (callable == module_state.copyreg_newobj) {
        if (PyTuple_GET_SIZE(argtup) < 1) {
            PyErr_Format(
                PyExc_TypeError,
                "__newobj__ expected at least 1 argument, got %zd",
                PyTuple_GET_SIZE(argtup)
            );
            goto error;
        }

        PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
        if (!PyType_Check(cls)) {
            PyErr_Format(
                PyExc_TypeError,
                "first argument to __newobj__() must be a class, not %.200s",
                Py_TYPE(cls)->tp_name
            );
            goto error;
        }

        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup) - 1;
        PyObject* newargs = PyTuple_New(nargs);
        if (!newargs)
            goto error;

        for (Py_ssize_t i = 0; i < nargs; i++) {
            PyObject* arg = PyTuple_GET_ITEM(argtup, i + 1);
            PyObject* arg_copy = deepcopy(arg, memo);
            if (!arg_copy) {
                Py_DECREF(newargs);
                goto error;
            }
            PyTuple_SET_ITEM(newargs, i, arg_copy);
        }

        inst = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, newargs, NULL);
        Py_DECREF(newargs);
        if (!inst)
            goto error;
    }
    // Handle __newobj_ex__
    else if (callable == module_state.copyreg_newobj_ex) {
        if (PyTuple_GET_SIZE(argtup) != 3) {
            PyErr_Format(
                PyExc_TypeError,
                "__newobj_ex__ expected 3 arguments, got %zd",
                PyTuple_GET_SIZE(argtup)
            );
            goto error;
        }

        PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
        PyObject* args = PyTuple_GET_ITEM(argtup, 1);
        PyObject* kwargs = PyTuple_GET_ITEM(argtup, 2);

        if (!PyType_Check(cls)) {
            PyErr_Format(
                PyExc_TypeError,
                "first argument to __newobj_ex__() must be a class, not %.200s",
                Py_TYPE(cls)->tp_name
            );
            goto error;
        }
        if (!PyTuple_Check(args)) {
            PyErr_Format(
                PyExc_TypeError,
                "second argument to __newobj_ex__() must be a tuple, not %.200s",
                Py_TYPE(args)->tp_name
            );
            goto error;
        }
        if (!PyDict_Check(kwargs)) {
            PyErr_Format(
                PyExc_TypeError,
                "third argument to __newobj_ex__() must be a dict, not %.200s",
                Py_TYPE(kwargs)->tp_name
            );
            goto error;
        }

        PyObject* args_copy = deepcopy(args, memo);
        if (!args_copy)
            goto error;

        PyObject* kwargs_copy = deepcopy(kwargs, memo);
        if (!kwargs_copy) {
            Py_DECREF(args_copy);
            goto error;
        }

        inst = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, args_copy, kwargs_copy);
        Py_DECREF(args_copy);
        Py_DECREF(kwargs_copy);
        if (!inst)
            goto error;
    }
    // Generic callable
    else {
        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup);
        if (nargs == 0) {
            inst = PyObject_CallNoArgs(callable);
        } else {
            PyObject* argtup_copy = PyTuple_New(nargs);
            if (!argtup_copy)
                goto error;

            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject* arg = PyTuple_GET_ITEM(argtup, i);
                PyObject* arg_copy = deepcopy(arg, memo);
                if (!arg_copy) {
                    Py_DECREF(argtup_copy);
                    goto error;
                }
                PyTuple_SET_ITEM(argtup_copy, i, arg_copy);
            }

            inst = PyObject_CallObject(callable, argtup_copy);
            Py_DECREF(argtup_copy);
        }
        if (!inst)
            goto error;
    }

    // Memoize early to handle self-referential structures
    if (memo_table_insert_h(&memo->table, (void*)obj, inst, id_hash) < 0)
        goto error;

    // Handle state (BUILD semantics)
    if (state) {
        if (PyObject_GetOptionalAttr(inst, module_state.str_setstate, &setstate) < 0)
            goto error;

        if (setstate) {
            // Explicit __setstate__
            state_copy = deepcopy(state, memo);
            if (!state_copy) {
                Py_DECREF(setstate);
                setstate = NULL;
                goto error;
            }

            result = PyObject_CallOneArg(setstate, state_copy);
            Py_DECREF(state_copy);
            state_copy = NULL;
            Py_DECREF(setstate);
            setstate = NULL;
            if (!result)
                goto error;
            Py_DECREF(result);
            result = NULL;
        } else {
            // Default __setstate__: handle inst.__dict__ and slotstate
            PyObject* dict_state = NULL;
            PyObject* slotstate = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slotstate = PyTuple_GET_ITEM(state, 1);
            } else {
                dict_state = state;
            }

            // Set inst.__dict__ from the state dict
            if (dict_state && dict_state != Py_None) {
                if (!PyDict_Check(dict_state)) {
                    PyErr_SetString(PyExc_TypeError, "state is not a dictionary");
                    goto error;
                }

                dict_state_copy = deepcopy(dict_state, memo);
                if (!dict_state_copy)
                    goto error;

                inst_dict = PyObject_GetAttr(inst, module_state.str_dict);
                if (!inst_dict) {
                    Py_DECREF(dict_state_copy);
                    dict_state_copy = NULL;
                    goto error;
                }

                PyObject *d_key, *d_value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(dict_state_copy, &pos, &d_key, &d_value)) {
                    if (PyObject_SetItem(inst_dict, d_key, d_value) < 0) {
                        Py_DECREF(inst_dict);
                        inst_dict = NULL;
                        Py_DECREF(dict_state_copy);
                        dict_state_copy = NULL;
                        goto error;
                    }
                }

                Py_DECREF(inst_dict);
                inst_dict = NULL;
                Py_DECREF(dict_state_copy);
                dict_state_copy = NULL;
            }

            // Also set instance attributes from the slotstate dict
            if (slotstate && slotstate != Py_None) {
                if (!PyDict_Check(slotstate)) {
                    PyErr_SetString(PyExc_TypeError, "slot state is not a dictionary");
                    goto error;
                }

                slotstate_copy = deepcopy(slotstate, memo);
                if (!slotstate_copy)
                    goto error;

                PyObject *d_key, *d_value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(slotstate_copy, &pos, &d_key, &d_value)) {
                    if (PyObject_SetAttr(inst, d_key, d_value) < 0) {
                        Py_DECREF(slotstate_copy);
                        slotstate_copy = NULL;
                        goto error;
                    }
                }

                Py_DECREF(slotstate_copy);
                slotstate_copy = NULL;
            }
        }
    }

    // Handle listitems
    if (listitems) {
        append = PyObject_GetAttr(inst, module_state.str_append);
        if (!append)
            goto error;

        it = PyObject_GetIter(listitems);
        if (!it) {
            Py_DECREF(append);
            append = NULL;
            goto error;
        }

        PyObject* loop_item;
        while ((loop_item = PyIter_Next(it)) != NULL) {
            item_copy = deepcopy(loop_item, memo);
            Py_DECREF(loop_item);
            if (!item_copy) {
                Py_DECREF(it);
                it = NULL;
                Py_DECREF(append);
                append = NULL;
                goto error;
            }

            result = PyObject_CallOneArg(append, item_copy);
            Py_DECREF(item_copy);
            item_copy = NULL;
            if (!result) {
                Py_DECREF(it);
                it = NULL;
                Py_DECREF(append);
                append = NULL;
                goto error;
            }
            Py_DECREF(result);
            result = NULL;
        }

        if (PyErr_Occurred()) {
            Py_DECREF(it);
            it = NULL;
            Py_DECREF(append);
            append = NULL;
            goto error;
        }

        Py_DECREF(it);
        it = NULL;
        Py_DECREF(append);
        append = NULL;
    }

    // Handle dictitems
    if (dictitems) {
        it = PyObject_GetIter(dictitems);
        if (!it)
            goto error;

        PyObject* loop_pair;
        while ((loop_pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(loop_pair) || PyTuple_GET_SIZE(loop_pair) != 2) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
                goto error;
            }

            PyObject* key = PyTuple_GET_ITEM(loop_pair, 0);
            PyObject* value = PyTuple_GET_ITEM(loop_pair, 1);

            key_copy = deepcopy(key, memo);
            if (!key_copy) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                goto error;
            }

            value_copy = deepcopy(value, memo);
            if (!value_copy) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                goto error;
            }

            Py_DECREF(loop_pair);

            int status = PyObject_SetItem(inst, key_copy, value_copy);
            Py_DECREF(key_copy);
            key_copy = NULL;
            Py_DECREF(value_copy);
            value_copy = NULL;

            if (status < 0) {
                Py_DECREF(it);
                it = NULL;
                goto error;
            }
        }

        if (PyErr_Occurred()) {
            Py_DECREF(it);
            it = NULL;
            goto error;
        }

        Py_DECREF(it);
        it = NULL;
    }

    // Keep alive original object if reconstruction returned different object
    if (inst != obj) {
        if (keepalive_append(&memo->keepalive, obj) < 0)
            goto error;
    }

    Py_DECREF(reduce_result);
    return inst;

error:
    Py_XDECREF(inst);
    Py_XDECREF(reduce_result);
    Py_XDECREF(setstate);
    Py_XDECREF(state_copy);
    Py_XDECREF(result);
    Py_XDECREF(dict_state_copy);
    Py_XDECREF(inst_dict);
    Py_XDECREF(slotstate_copy);
    Py_XDECREF(append);
    Py_XDECREF(it);
    Py_XDECREF(item_copy);
    Py_XDECREF(key_copy);
    Py_XDECREF(value_copy);
    return NULL;
}

/* === Python-dict memo specializations ====================================== */

// Keep-append helper for Python-dict memo path.
// Returns 0 on success, -1 on failure. Caller handles cleanup.
static ALWAYS_INLINE int maybe_keepalive_legacy(
    PyObject* copy, PyObject* src, PyObject* memo, PyObject** keepalive_pointer
) {
    if (copy != src) {
        return keepalive_legacy(memo, keepalive_pointer, src);
    }
    return 0;
}

static MAYBE_INLINE PyObject* deepcopy_list_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_tuple_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_dict_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_set_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_frozenset_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_bytearray_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static MAYBE_INLINE PyObject* deepcopy_method_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
);
static PyObject* deepcopy_object_legacy(
    PyObject* obj,
    PyTypeObject* tp,
    PyObject* memo,
    PyObject** keepalive_pointer,
    Py_ssize_t id_hash
);

static ALWAYS_INLINE PyObject* deepcopy_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer
) {
    PyTypeObject* tp = Py_TYPE(obj);

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    /* Python 3.14+: Check ALL atomic immutables before memo lookup */
    if (LIKELY(is_literal_immutable(tp) || is_builtin_immutable(tp) || is_class(tp))) {
        return Py_NewRef(obj);
    }
#endif

    /* Memo lookup (Python < 3.14: this happens before immutable check) */
    void* id = (void*)obj;
    Py_ssize_t h = memo_hash_pointer(id);
    PyObject* hit = memo_lookup_legacy(memo, id);
    if (hit)
        return hit;
    if (PyErr_Occurred())
        return NULL;

#if PY_VERSION_HEX < PY_VERSION_3_14_HEX
    /* Python < 3.14: Check immutables after memo lookup */
    if (LIKELY(is_literal_immutable(tp))) {
        return Py_NewRef(obj);
    }
#endif

    /* 3) Popular containers first (specialized, likely hot) */
    if (tp == &PyList_Type)
        return RECURSION_GUARDED(deepcopy_list_legacy(obj, memo, keepalive_pointer, h));
    if (tp == &PyTuple_Type)
        return RECURSION_GUARDED(deepcopy_tuple_legacy(obj, memo, keepalive_pointer, h));
    if (tp == &PyDict_Type)
        return RECURSION_GUARDED(deepcopy_dict_legacy(obj, memo, keepalive_pointer, h));
    if (tp == &PySet_Type)
        return RECURSION_GUARDED(deepcopy_set_legacy(obj, memo, keepalive_pointer, h));

    if (is_builtin_immutable(tp) || is_class(tp)) {
        return Py_NewRef(obj);
    }

    if (tp == &PyFrozenSet_Type)
        return deepcopy_frozenset_legacy(obj, memo, keepalive_pointer, h);
    if (tp == &PyByteArray_Type)
        return deepcopy_bytearray_legacy(obj, memo, keepalive_pointer, h);
    if (tp == &PyMethod_Type)
        return deepcopy_method_legacy(obj, memo, keepalive_pointer, h);

    if (is_stdlib_immutable(tp)) {
        return Py_NewRef(obj);
    }

    /* Robustly detect a user-defined __deepcopy__ via optional lookup (single step, non-raising on miss). */
    {
        PyObject* deepcopy_meth = NULL;
        int has_deepcopy = PyObject_GetOptionalAttr(obj, module_state.str_deepcopy, &deepcopy_meth);
        if (has_deepcopy < 0)
            return NULL;
        if (has_deepcopy) {
            PyObject* res = PyObject_CallOneArg(deepcopy_meth, memo);
            Py_DECREF(deepcopy_meth);
            if (!res)
                return NULL;
            if (res != obj) {
                if (memoize_legacy(memo, (void*)obj, res) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
                if (keepalive_legacy(memo, keepalive_pointer, obj) < 0) {
                    Py_DECREF(res);
                    return NULL;
                }
            }
            return res;
        }
    }

    PyObject* res = deepcopy_object_legacy(obj, tp, memo, keepalive_pointer, h);
    return res;
}

static MAYBE_INLINE PyObject* deepcopy_list_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* copied_item = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = PyList_New(sz);
    if (!copy)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i)
        PyList_SET_ITEM(copy, i, Py_NewRef(Py_None));

    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error;

    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyList_GET_ITEM(obj, i);
        copied_item = deepcopy_legacy(item, memo, keepalive_pointer);
        if (!copied_item)
            goto error;
        Py_DECREF(Py_None);
        PyList_SET_ITEM(copy, i, copied_item);
        copied_item = NULL;  // ownership transferred to list
    }

    // If the source list grew during the first pass, append extra tail items.
    Py_ssize_t i2 = sz;
    while (i2 < Py_SIZE(obj)) {
        PyObject* item2 = PyList_GET_ITEM(obj, i2);
        copied_item = deepcopy_legacy(item2, memo, keepalive_pointer);
        if (!copied_item)
            goto error;
        if (PyList_Append(copy, copied_item) < 0)
            goto error;
        Py_DECREF(copied_item);
        copied_item = NULL;
        i2++;
    }
    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error;
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(copied_item);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_tuple_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* copied = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = PyTuple_New(sz);
    if (!copy)
        goto error;

    int all_same = 1;
    for (Py_ssize_t i = 0; i < sz; ++i) {
        PyObject* item = PyTuple_GET_ITEM(obj, i);
        copied = deepcopy_legacy(item, memo, keepalive_pointer);
        if (!copied)
            goto error;
        if (copied != item)
            all_same = 0;
        PyTuple_SET_ITEM(copy, i, copied);
        copied = NULL;  // ownership transferred to tuple
    }
    if (all_same) {
        Py_DECREF(copy);
        return Py_NewRef(obj);
    }

    /* Handle self-referential tuples: if a recursive path already created a copy,
       prefer that existing copy to maintain identity. */
    PyObject* existing = memo_lookup_legacy(memo, (void*)obj);
    if (existing) {
        Py_DECREF(copy);
        return existing;
    }
    if (PyErr_Occurred())
        goto error;

    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error;
    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error;
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(copied);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_dict_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* ckey = NULL;
    PyObject* cvalue = NULL;

    Py_ssize_t sz = Py_SIZE(obj);
    copy = _PyDict_NewPresized(sz);
    if (!copy)
        goto error_no_cleanup;
    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error_no_cleanup;

    // NOTE: iter_guard is declared here, after early returns.
    // error_no_cleanup is for paths before iter_guard is initialized or after dict_iter_next
    // already cleaned up. error is for mid-iteration cleanup (3.14+ only).
    DictIterGuard iter_guard;
    dict_iter_init(&iter_guard, obj);

    PyObject *key, *value;
    int ret;
    while ((ret = dict_iter_next(&iter_guard, &key, &value)) > 0) {
        ckey = deepcopy_legacy(key, memo, keepalive_pointer);
        if (!ckey)
            goto error;
        cvalue = deepcopy_legacy(value, memo, keepalive_pointer);
        if (!cvalue)
            goto error;
        if (PyDict_SetItem(copy, ckey, cvalue) < 0)
            goto error;
        Py_DECREF(ckey);
        ckey = NULL;
        Py_DECREF(cvalue);
        cvalue = NULL;
    }
    if (ret < 0)  // mutation detected -> error already set, cleanup done
        goto error_no_cleanup;

    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error_no_cleanup;
    return copy;

error:
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    dict_iter_cleanup(&iter_guard);
#endif
error_no_cleanup:
    Py_XDECREF(copy);
    Py_XDECREF(ckey);
    Py_XDECREF(cvalue);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_set_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* copy = NULL;
    PyObject* snap = NULL;
    PyObject* citem = NULL;

    copy = PySet_New(NULL);
    if (!copy)
        goto error;
    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error;

    /* Snapshot into a pre-sized tuple without invoking user code. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        goto error;
    snap = PyTuple_New(n);
    if (!snap)
        goto error;

    Py_ssize_t pos = 0;
    PyObject* item;
    Py_hash_t hash;
    Py_ssize_t i = 0;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        if (i < n) {
            /* item is borrowed; store owned ref in the tuple */
            Py_INCREF(item);
            PyTuple_SET_ITEM(snap, i, item);
            i++;
        } else {
            /* If the set grows during snapshotting, ignore extras to avoid overflow. */
        }
    }

    /* Deepcopy from the stable snapshot prefix. */
    for (Py_ssize_t j = 0; j < i; j++) {
        PyObject* elem = PyTuple_GET_ITEM(snap, j); /* borrowed from snap */
        citem = deepcopy_legacy(elem, memo, keepalive_pointer);
        if (!citem)
            goto error;
        if (PySet_Add(copy, citem) < 0)
            goto error;
        Py_DECREF(citem);
        citem = NULL;
    }
    Py_DECREF(snap);

    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error;
    return copy;

error:
    Py_XDECREF(copy);
    Py_XDECREF(snap);
    Py_XDECREF(citem);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_frozenset_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* temp = NULL;
    PyObject* copy = NULL;
    PyObject* citem = NULL;

    /* Pre-size snapshot: frozenset is immutable, so size won't change mid-loop. */
    Py_ssize_t n = PySet_Size(obj);
    if (n == -1)
        goto error;

    temp = PyTuple_New(n);
    if (!temp)
        goto error;

    Py_ssize_t pos = 0, i = 0;
    PyObject* item;
    Py_hash_t hash;

    while (_PySet_NextEntry(obj, &pos, &item, &hash)) {
        /* item is borrowed; deepcopy_py returns a new reference which tuple will own. */
        citem = deepcopy_legacy(item, memo, keepalive_pointer);
        if (!citem)
            goto error;
        PyTuple_SET_ITEM(temp, i, citem);  // steals reference to citem
        citem = NULL;                      // ownership transferred
        i++;
    }

    copy = PyFrozenSet_New(temp);
    Py_DECREF(temp);
    temp = NULL;
    if (!copy)
        goto error;
    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error;
    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error;
    return copy;

error:
    Py_XDECREF(temp);
    Py_XDECREF(copy);
    Py_XDECREF(citem);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_bytearray_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    PyObject* copy = NULL;

    Py_ssize_t sz = PyByteArray_Size(obj);
    copy = PyByteArray_FromStringAndSize(NULL, sz);
    if (!copy)
        goto error;
    if (sz)
        memcpy(PyByteArray_AS_STRING(copy), PyByteArray_AS_STRING(obj), (size_t)sz);
    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error;
    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error;
    return copy;

error:
    Py_XDECREF(copy);
    return NULL;
}

static MAYBE_INLINE PyObject* deepcopy_method_legacy(
    PyObject* obj, PyObject* memo, PyObject** keepalive_pointer, Py_ssize_t id_hash
) {
    // Owned refs that need cleanup on error
    PyObject* func = NULL;
    PyObject* self = NULL;
    PyObject* cself = NULL;
    PyObject* copy = NULL;

    func = PyMethod_GET_FUNCTION(obj);
    self = PyMethod_GET_SELF(obj);
    if (!func || !self)
        goto error;

    Py_INCREF(func);
    Py_INCREF(self);
    cself = deepcopy_legacy(self, memo, keepalive_pointer);
    Py_DECREF(self);
    self = NULL;
    if (!cself)
        goto error;

    copy = PyMethod_New(func, cself);
    Py_DECREF(func);
    func = NULL;
    Py_DECREF(cself);
    cself = NULL;
    if (!copy)
        goto error;

    if (memoize_legacy(memo, (void*)obj, copy) < 0)
        goto error;
    if (maybe_keepalive_legacy(copy, obj, memo, keepalive_pointer) < 0)
        goto error;
    return copy;

error:
    Py_XDECREF(func);
    Py_XDECREF(self);
    Py_XDECREF(cself);
    Py_XDECREF(copy);
    return NULL;
}

// Reduce protocol implementation for Python-dict memo path.
// This is the cold fallback path - not marked ALWAYS_INLINE to avoid code bloat.
static PyObject* deepcopy_object_legacy(
    PyObject* obj,
    PyTypeObject* tp,
    PyObject* memo,
    PyObject** keepalive_pointer,
    Py_ssize_t id_hash
) {
    // All owned refs declared at top for cleanup
    PyObject* reduce_result = NULL;
    PyObject* inst = NULL;
    PyObject* setstate = NULL;
    PyObject* state_copy = NULL;
    PyObject* result = NULL;
    PyObject* dict_state_copy = NULL;
    PyObject* inst_dict = NULL;
    PyObject* slotstate_copy = NULL;
    PyObject* append = NULL;
    PyObject* it = NULL;
    PyObject* item_copy = NULL;
    PyObject* key_copy = NULL;
    PyObject* value_copy = NULL;

    reduce_result = try_reduce_via_registry(obj, tp);
    if (!reduce_result) {
        if (PyErr_Occurred())
            goto error;
        reduce_result = call_reduce_method_preferring_ex(obj);
        if (!reduce_result)
            goto error;
    }

    PyObject *callable, *argtup, *state, *listitems, *dictitems;
    int valid =
        validate_reduce_tuple(reduce_result, &callable, &argtup, &state, &listitems, &dictitems);
    if (valid == REDUCE_ERROR)
        goto error;
    if (valid == REDUCE_STRING) {
        Py_DECREF(reduce_result);
        return Py_NewRef(obj);
    }

    // Handle __newobj__
    if (callable == module_state.copyreg_newobj) {
        if (PyTuple_GET_SIZE(argtup) < 1) {
            PyErr_Format(
                PyExc_TypeError,
                "__newobj__ expected at least 1 argument, got %zd",
                PyTuple_GET_SIZE(argtup)
            );
            goto error;
        }

        PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
        if (!PyType_Check(cls)) {
            PyErr_Format(
                PyExc_TypeError,
                "first argument to __newobj__() must be a class, not %.200s",
                Py_TYPE(cls)->tp_name
            );
            goto error;
        }

        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup) - 1;
        PyObject* newargs = PyTuple_New(nargs);
        if (!newargs)
            goto error;

        for (Py_ssize_t i = 0; i < nargs; i++) {
            PyObject* arg = PyTuple_GET_ITEM(argtup, i + 1);
            PyObject* arg_copy = deepcopy_legacy(arg, memo, keepalive_pointer);
            if (!arg_copy) {
                Py_DECREF(newargs);
                goto error;
            }
            PyTuple_SET_ITEM(newargs, i, arg_copy);
        }

        inst = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, newargs, NULL);
        Py_DECREF(newargs);
        if (!inst)
            goto error;
    }
    // Handle __newobj_ex__
    else if (callable == module_state.copyreg_newobj_ex) {
        if (PyTuple_GET_SIZE(argtup) != 3) {
            PyErr_Format(
                PyExc_TypeError,
                "__newobj_ex__ expected 3 arguments, got %zd",
                PyTuple_GET_SIZE(argtup)
            );
            goto error;
        }

        PyObject* cls = PyTuple_GET_ITEM(argtup, 0);
        PyObject* args = PyTuple_GET_ITEM(argtup, 1);
        PyObject* kwargs = PyTuple_GET_ITEM(argtup, 2);

        if (!PyType_Check(cls)) {
            PyErr_Format(
                PyExc_TypeError,
                "first argument to __newobj_ex__() must be a class, not %.200s",
                Py_TYPE(cls)->tp_name
            );
            goto error;
        }
        if (!PyTuple_Check(args)) {
            PyErr_Format(
                PyExc_TypeError,
                "second argument to __newobj_ex__() must be a tuple, not %.200s",
                Py_TYPE(args)->tp_name
            );
            goto error;
        }
        if (!PyDict_Check(kwargs)) {
            PyErr_Format(
                PyExc_TypeError,
                "third argument to __newobj_ex__() must be a dict, not %.200s",
                Py_TYPE(kwargs)->tp_name
            );
            goto error;
        }

        PyObject* args_copy = deepcopy_legacy(args, memo, keepalive_pointer);
        if (!args_copy)
            goto error;

        PyObject* kwargs_copy = deepcopy_legacy(kwargs, memo, keepalive_pointer);
        if (!kwargs_copy) {
            Py_DECREF(args_copy);
            goto error;
        }

        inst = ((PyTypeObject*)cls)->tp_new((PyTypeObject*)cls, args_copy, kwargs_copy);
        Py_DECREF(args_copy);
        Py_DECREF(kwargs_copy);
        if (!inst)
            goto error;
    }
    // Generic callable
    else {
        Py_ssize_t nargs = PyTuple_GET_SIZE(argtup);
        if (nargs == 0) {
            inst = PyObject_CallNoArgs(callable);
        } else {
            PyObject* argtup_copy = PyTuple_New(nargs);
            if (!argtup_copy)
                goto error;

            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject* arg = PyTuple_GET_ITEM(argtup, i);
                PyObject* arg_copy = deepcopy_legacy(arg, memo, keepalive_pointer);
                if (!arg_copy) {
                    Py_DECREF(argtup_copy);
                    goto error;
                }
                PyTuple_SET_ITEM(argtup_copy, i, arg_copy);
            }

            inst = PyObject_CallObject(callable, argtup_copy);
            Py_DECREF(argtup_copy);
        }
        if (!inst)
            goto error;
    }

    // Memoize early to handle self-referential structures
    if (memoize_legacy(memo, (void*)obj, inst) < 0)
        goto error;

    // Handle state (BUILD semantics)
    if (state) {
        if (PyObject_GetOptionalAttr(inst, module_state.str_setstate, &setstate) < 0)
            goto error;

        if (setstate) {
            // Explicit __setstate__
            state_copy = deepcopy_legacy(state, memo, keepalive_pointer);
            if (!state_copy) {
                Py_DECREF(setstate);
                setstate = NULL;
                goto error;
            }

            result = PyObject_CallOneArg(setstate, state_copy);
            Py_DECREF(state_copy);
            state_copy = NULL;
            Py_DECREF(setstate);
            setstate = NULL;
            if (!result)
                goto error;
            Py_DECREF(result);
            result = NULL;
        } else {
            // Default __setstate__: handle inst.__dict__ and slotstate
            PyObject* dict_state = NULL;
            PyObject* slotstate = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slotstate = PyTuple_GET_ITEM(state, 1);
            } else {
                dict_state = state;
            }

            // Set inst.__dict__ from the state dict
            if (dict_state && dict_state != Py_None) {
                if (!PyDict_Check(dict_state)) {
                    PyErr_SetString(PyExc_TypeError, "state is not a dictionary");
                    goto error;
                }

                dict_state_copy = deepcopy_legacy(dict_state, memo, keepalive_pointer);
                if (!dict_state_copy)
                    goto error;

                inst_dict = PyObject_GetAttr(inst, module_state.str_dict);
                if (!inst_dict) {
                    Py_DECREF(dict_state_copy);
                    dict_state_copy = NULL;
                    goto error;
                }

                PyObject *d_key, *d_value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(dict_state_copy, &pos, &d_key, &d_value)) {
                    if (PyObject_SetItem(inst_dict, d_key, d_value) < 0) {
                        Py_DECREF(inst_dict);
                        inst_dict = NULL;
                        Py_DECREF(dict_state_copy);
                        dict_state_copy = NULL;
                        goto error;
                    }
                }

                Py_DECREF(inst_dict);
                inst_dict = NULL;
                Py_DECREF(dict_state_copy);
                dict_state_copy = NULL;
            }

            // Also set instance attributes from the slotstate dict
            if (slotstate && slotstate != Py_None) {
                if (!PyDict_Check(slotstate)) {
                    PyErr_SetString(PyExc_TypeError, "slot state is not a dictionary");
                    goto error;
                }

                slotstate_copy = deepcopy_legacy(slotstate, memo, keepalive_pointer);
                if (!slotstate_copy)
                    goto error;

                PyObject *d_key, *d_value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(slotstate_copy, &pos, &d_key, &d_value)) {
                    if (PyObject_SetAttr(inst, d_key, d_value) < 0) {
                        Py_DECREF(slotstate_copy);
                        slotstate_copy = NULL;
                        goto error;
                    }
                }

                Py_DECREF(slotstate_copy);
                slotstate_copy = NULL;
            }
        }
    }

    // Handle listitems
    if (listitems) {
        append = PyObject_GetAttr(inst, module_state.str_append);
        if (!append)
            goto error;

        it = PyObject_GetIter(listitems);
        if (!it) {
            Py_DECREF(append);
            append = NULL;
            goto error;
        }

        PyObject* loop_item;
        while ((loop_item = PyIter_Next(it)) != NULL) {
            item_copy = deepcopy_legacy(loop_item, memo, keepalive_pointer);
            Py_DECREF(loop_item);
            if (!item_copy) {
                Py_DECREF(it);
                it = NULL;
                Py_DECREF(append);
                append = NULL;
                goto error;
            }

            result = PyObject_CallOneArg(append, item_copy);
            Py_DECREF(item_copy);
            item_copy = NULL;
            if (!result) {
                Py_DECREF(it);
                it = NULL;
                Py_DECREF(append);
                append = NULL;
                goto error;
            }
            Py_DECREF(result);
            result = NULL;
        }

        if (PyErr_Occurred()) {
            Py_DECREF(it);
            it = NULL;
            Py_DECREF(append);
            append = NULL;
            goto error;
        }

        Py_DECREF(it);
        it = NULL;
        Py_DECREF(append);
        append = NULL;
    }

    // Handle dictitems
    if (dictitems) {
        it = PyObject_GetIter(dictitems);
        if (!it)
            goto error;

        PyObject* loop_pair;
        while ((loop_pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(loop_pair) || PyTuple_GET_SIZE(loop_pair) != 2) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
                goto error;
            }

            PyObject* key = PyTuple_GET_ITEM(loop_pair, 0);
            PyObject* value = PyTuple_GET_ITEM(loop_pair, 1);

            key_copy = deepcopy_legacy(key, memo, keepalive_pointer);
            if (!key_copy) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                goto error;
            }

            value_copy = deepcopy_legacy(value, memo, keepalive_pointer);
            if (!value_copy) {
                Py_DECREF(loop_pair);
                Py_DECREF(it);
                it = NULL;
                goto error;
            }

            Py_DECREF(loop_pair);

            int status = PyObject_SetItem(inst, key_copy, value_copy);
            Py_DECREF(key_copy);
            key_copy = NULL;
            Py_DECREF(value_copy);
            value_copy = NULL;

            if (status < 0) {
                Py_DECREF(it);
                it = NULL;
                goto error;
            }
        }

        if (PyErr_Occurred()) {
            Py_DECREF(it);
            it = NULL;
            goto error;
        }

        Py_DECREF(it);
        it = NULL;
    }

    // Keep alive original object if reconstruction returned different object
    if (inst != obj) {
        if (keepalive_legacy(memo, keepalive_pointer, obj) < 0)
            goto error;
    }

    Py_DECREF(reduce_result);
    return inst;

error:
    Py_XDECREF(inst);
    Py_XDECREF(reduce_result);
    Py_XDECREF(setstate);
    Py_XDECREF(state_copy);
    Py_XDECREF(result);
    Py_XDECREF(dict_state_copy);
    Py_XDECREF(inst_dict);
    Py_XDECREF(slotstate_copy);
    Py_XDECREF(append);
    Py_XDECREF(it);
    Py_XDECREF(item_copy);
    Py_XDECREF(key_copy);
    Py_XDECREF(value_copy);
    return NULL;
}

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

static PyObject* reconstruct_state(
    PyObject* new_obj, PyObject* state, PyObject* listiter, PyObject* dictiter
) {
    if (UNLIKELY(new_obj == NULL)) {
        PyErr_SetString(PyExc_SystemError, "reconstruct_state: new_obj is NULL");
        return NULL;
    }
    if (!state)
        state = Py_None;
    if (!listiter)
        listiter = Py_None;
    if (!dictiter)
        dictiter = Py_None;

    if (state != Py_None) {
        PyObject* setstate = PyObject_GetAttr(new_obj, module_state.str_setstate);
        if (setstate) {
            PyObject* r = PyObject_CallOneArg(setstate, state);
            Py_DECREF(setstate);
            if (!r)
                return NULL;
            Py_DECREF(r);
        } else {
            PyErr_Clear();
            PyObject* dict_state = NULL;
            PyObject* slot_state = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slot_state = PyTuple_GET_ITEM(state, 1);

                if (slot_state && slot_state != Py_None) {
                    PyObject* it = PyObject_GetIter(slot_state);
                    if (!it)
                        return NULL;
                    PyObject* key;
                    while ((key = PyIter_Next(it)) != NULL) {
                        PyObject* value = PyObject_GetItem(slot_state, key);
                        if (!value) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            return NULL;
                        }
                        if (PyObject_SetAttr(new_obj, key, value) < 0) {
                            Py_DECREF(value);
                            Py_DECREF(key);
                            Py_DECREF(it);
                            return NULL;
                        }
                        Py_DECREF(value);
                        Py_DECREF(key);
                    }
                    Py_DECREF(it);
                    if (PyErr_Occurred())
                        return NULL;
                }
            } else {
                dict_state = state;  // treat as mapping-like
            }

            if (dict_state && dict_state != Py_None) {
                PyObject* obj_dict = PyObject_GetAttr(new_obj, module_state.str_dict);
                if (!obj_dict)
                    return NULL;
                PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
                Py_DECREF(obj_dict);
                if (!update)
                    return NULL;
                PyObject* r = PyObject_CallOneArg(update, dict_state);
                Py_DECREF(update);
                if (!r)
                    return NULL;
                Py_DECREF(r);
            }
        }
    }

    if (listiter != Py_None) {
        PyObject* append = PyObject_GetAttr(new_obj, module_state.str_append);
        if (!append)
            return NULL;
        PyObject* it = PyObject_GetIter(listiter);
        if (!it) {
            Py_DECREF(append);
            return NULL;
        }
        PyObject* item;
        while ((item = PyIter_Next(it)) != NULL) {
            PyObject* r = PyObject_CallOneArg(append, item);
            Py_DECREF(item);
            if (!r) {
                Py_DECREF(it);
                Py_DECREF(append);
                return NULL;
            }
            Py_DECREF(r);
        }
        Py_DECREF(it);
        Py_DECREF(append);
        if (PyErr_Occurred())
            return NULL;
    }

    if (dictiter != Py_None) {
        PyObject* it = PyObject_GetIter(dictiter);
        if (!it)
            return NULL;
        PyObject* pair;
        while ((pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
                Py_DECREF(pair);
                Py_DECREF(it);
                PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
                return NULL;
            }
            PyObject* k = PyTuple_GET_ITEM(pair, 0);
            PyObject* v = PyTuple_GET_ITEM(pair, 1);
            Py_INCREF(k);
            Py_INCREF(v);
            if (PyObject_SetItem(new_obj, k, v) < 0) {
                Py_DECREF(k);
                Py_DECREF(v);
                Py_DECREF(pair);
                Py_DECREF(it);
                return NULL;
            }
            Py_DECREF(k);
            Py_DECREF(v);
            Py_DECREF(pair);
        }
        Py_DECREF(it);
        if (PyErr_Occurred())
            return NULL;
    }

    return Py_NewRef(new_obj);
}

/* -------------------------------- copy() ---------------------------------- */

static ALWAYS_INLINE int is_empty_initializable(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyList_Type)
        return Py_SIZE(obj) == 0;
    if (tp == &PyTuple_Type)
        return Py_SIZE(obj) == 0;
    if (tp == &PyDict_Type)
        return PyDict_Size(obj) == 0;
    if (tp == &PySet_Type)
        return PySet_Size(obj) == 0;
    if (tp == &PyFrozenSet_Type)
        return PyObject_Size(obj) == 0;
    if (tp == &PyByteArray_Type)
        return PyByteArray_Size(obj) == 0;
    return 0;
}

static PyObject* make_empty_same_type(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyList_Type)
        return PyList_New(0);
    if (tp == &PyTuple_Type)
        return PyTuple_New(0);
    if (tp == &PyDict_Type)
        return PyDict_New();
    if (tp == &PySet_Type)
        return PySet_New(NULL);
    if (tp == &PyFrozenSet_Type)
        return PyFrozenSet_New(NULL);
    if (tp == &PyByteArray_Type)
        return PyByteArray_FromStringAndSize(NULL, 0);
    Py_RETURN_NONE;  // shouldn't happen; caller guards
}

static PyObject* try_stdlib_mutable_copy(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyDict_Type || tp == &PySet_Type || tp == &PyList_Type || tp == &PyByteArray_Type) {
        PyObject* method = PyObject_GetAttrString(obj, "copy");
        if (!method) {
            PyErr_Clear();
        } else {
            PyObject* out = PyObject_CallNoArgs(method);
            Py_DECREF(method);
            if (out)
                return out;
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

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

/* -------------------------------- replace() --------------------------------
 */

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
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    if (PyThread_tss_is_created(&g_dictiter_tss)) {
        PyThread_tss_delete(&g_dictiter_tss);
    }
    if (g_dict_watcher_registered) {
        PyDict_ClearWatcher(g_dict_watcher_id);
        g_dict_watcher_registered = 0;
        g_dict_watcher_id = -1;
    }
#endif
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

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    // Create TLS for dict-iteration watcher stack
    if (PyThread_tss_create(&g_dictiter_tss) != 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to create dict-iteration TSS");
        goto init_error;
    }
    g_dict_watcher_id = PyDict_AddWatcher(_copium_dict_watcher_cb);
    if (g_dict_watcher_id < 0) {
        PyErr_SetString(PyExc_ImportError, "copium: failed to register dict watcher");
        goto init_error;
    }
    g_dict_watcher_registered = 1;
#endif

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
