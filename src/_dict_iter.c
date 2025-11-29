#ifndef _COPIUM_DICT_ITER_C
#define _COPIUM_DICT_ITER_C

#include "copium_common.h"

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
static Py_tss_t g_dictiter_tss = Py_tss_NEEDS_INIT;
static int g_dict_watcher_id = -1;
static int g_dict_watcher_registered = 0;

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

static int dict_iter_module_init(void) {
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    if (PyThread_tss_create(&g_dictiter_tss) != 0)
        return -1;
    g_dict_watcher_id = PyDict_AddWatcher(_copium_dict_watcher_cb);
    if (g_dict_watcher_id < 0)
        return -1;
    g_dict_watcher_registered = 1;
#endif
    return 0;
}

static void dict_iter_module_cleanup(void) {
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
#endif  // _COPIUM_DICT_ITER_C
