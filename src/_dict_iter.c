/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_DICT_ITER_C
#define _COPIUM_DICT_ITER_C

#include "_common.h"
#include "_state.c"

#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    #include <stdatomic.h>

static PyMutex g_dictiter_mutex = {0};
static int g_dict_watcher_id = -1;
static int g_dict_watcher_registered = 0;

    #ifdef Py_GIL_DISABLED

typedef struct DictIterGuard {
    PyObject* dict;
    PyObject* it;
} DictIterGuard;

static ALWAYS_INLINE int dict_iter_init(DictIterGuard* iter_guard, PyObject* dict) {
    iter_guard->dict = dict;
    iter_guard->it = NULL;
    PyObject* args[1] = {dict};
    PyObject* items_view = module_state.dict_items_vc(
        module_state.dict_items_descr, args, PyVectorcall_NARGS(1), NULL
    );
    if (!items_view)
        return -1;
    iter_guard->it = Py_TYPE(items_view)->tp_iter(items_view);
    Py_DECREF(items_view);
    return 0;
}

static ALWAYS_INLINE void dict_iter_cleanup(DictIterGuard* iter_guard) {
    Py_CLEAR(iter_guard->it);
}

static ALWAYS_INLINE int dict_iter_next(
    DictIterGuard* iter_guard, PyObject** key, PyObject** value
) {
    PyObject* item;
    int result = PyIter_NextItem(iter_guard->it, &item);
    if (result != 1) {
        dict_iter_cleanup(iter_guard);
        return result;
    }
    *key = Py_NewRef(PyTuple_GET_ITEM(item, 0));
    *value = Py_NewRef(PyTuple_GET_ITEM(item, 1));
    Py_DECREF(item);
    return 1;
}

    #else  // !PY_GIL_DISABLED

typedef struct DictIterGuard {
    PyObject* dict;
    Py_ssize_t pos;
    Py_ssize_t size0;
    atomic_int mutated;
    int size_changed;
    int last_event;
    struct DictIterGuard* prev;
    struct DictIterGuard* next;
} DictIterGuard;

static DictIterGuard* g_guard_list_head = NULL;

static int _copium_dict_watcher_cb(
    PyDict_WatchEvent event, PyObject* dict, PyObject* key, PyObject* new_value
) {
    (void)key;
    (void)new_value;

    PyMutex_Lock(&g_dictiter_mutex);
    for (DictIterGuard* g = g_guard_list_head; g; g = g->next) {
        if (g->dict == dict) {
            g->last_event = (int)event;

            if (event == PyDict_EVENT_ADDED || event == PyDict_EVENT_DELETED ||
                event == PyDict_EVENT_CLEARED || event == PyDict_EVENT_CLONED) {
                g->size_changed = 1;
            } else {
                Py_ssize_t cur = PyDict_Size(dict);
                if (cur >= 0 && cur != g->size0) {
                    g->size_changed = 1;
                }
            }
            atomic_store_explicit(&g->mutated, 1, memory_order_release);
        }
    }
    PyMutex_Unlock(&g_dictiter_mutex);
    return 0;
}

static int dict_watch_count_locked(PyObject* dict) {
    int count = 0;
    for (DictIterGuard* g = g_guard_list_head; g; g = g->next) {
        if (g->dict == dict)
            count++;
    }
    return count;
}

static ALWAYS_INLINE int dict_iter_init(DictIterGuard* iter_guard, PyObject* dict) {
    iter_guard->dict = dict;
    iter_guard->pos = 0;
    iter_guard->size0 = PyDict_Size(dict);
    atomic_init(&iter_guard->mutated, 0);
    iter_guard->size_changed = 0;
    iter_guard->last_event = 0;
    iter_guard->prev = NULL;
    iter_guard->next = NULL;

    PyMutex_Lock(&g_dictiter_mutex);
    int need_watch = (dict_watch_count_locked(dict) == 0);
    iter_guard->next = g_guard_list_head;
    if (g_guard_list_head) {
        g_guard_list_head->prev = iter_guard;
    }
    g_guard_list_head = iter_guard;
    if (need_watch && g_dict_watcher_registered) {
        PyDict_Watch(g_dict_watcher_id, dict);
    }
    PyMutex_Unlock(&g_dictiter_mutex);

    return 0;
}

static ALWAYS_INLINE void dict_iter_cleanup(DictIterGuard* iter_guard) {
    PyObject* dict = iter_guard->dict;

    PyMutex_Lock(&g_dictiter_mutex);
    if (iter_guard->prev) {
        iter_guard->prev->next = iter_guard->next;
    } else {
        g_guard_list_head = iter_guard->next;
    }
    if (iter_guard->next) {
        iter_guard->next->prev = iter_guard->prev;
    }
    int need_unwatch = (dict_watch_count_locked(dict) == 0);
    if (need_unwatch && g_dict_watcher_registered) {
        PyDict_Unwatch(g_dict_watcher_id, dict);
    }
    PyMutex_Unlock(&g_dictiter_mutex);
}

static ALWAYS_INLINE int dict_iter_next(
    DictIterGuard* iter_guard, PyObject** key, PyObject** value
) {
    if (PyDict_Next(iter_guard->dict, &iter_guard->pos, key, value)) {
        if (UNLIKELY(atomic_load_explicit(&iter_guard->mutated, memory_order_acquire))) {
            int size_changed_now = 0;
            Py_ssize_t cur = PyDict_Size(iter_guard->dict);
            if (cur >= 0) {
                size_changed_now = (cur != iter_guard->size0);
            } else {
                PyMutex_Lock(&g_dictiter_mutex);
                size_changed_now = iter_guard->size_changed;
                PyMutex_Unlock(&g_dictiter_mutex);
            }
            PyErr_SetString(
                PyExc_RuntimeError,
                size_changed_now ? "dictionary changed size during iteration"
                                 : "dictionary keys changed during iteration"
            );
            dict_iter_cleanup(iter_guard);
            return -1;
        }
        Py_INCREF(*key);
        Py_INCREF(*value);
        return 1;
    }

    if (UNLIKELY(atomic_load_explicit(&iter_guard->mutated, memory_order_acquire))) {
        int size_changed_now = 0;
        Py_ssize_t cur = PyDict_Size(iter_guard->dict);
        if (cur >= 0) {
            size_changed_now = (cur != iter_guard->size0);
        } else {
            PyMutex_Lock(&g_dictiter_mutex);
            size_changed_now = iter_guard->size_changed;
            PyMutex_Unlock(&g_dictiter_mutex);
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
    #endif
#else

typedef struct {
    PyObject* dict;
    Py_ssize_t pos;
    uint64_t ver0;
    Py_ssize_t used0;
} DictIterGuard;

static ALWAYS_INLINE int dict_iter_init(DictIterGuard* iter_guard, PyObject* dict) {
    iter_guard->dict = dict;
    iter_guard->pos = 0;
    iter_guard->ver0 = ((PyDictObject*)dict)->ma_version_tag;
    iter_guard->used0 = ((PyDictObject*)dict)->ma_used;
    return 0;
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
        Py_INCREF(*key);
        Py_INCREF(*value);
        return 1;
    }
    return 0;
}
#endif

static int dict_iter_module_init(void) {
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    #ifndef Py_GIL_DISABLED
    g_dict_watcher_id = PyDict_AddWatcher(_copium_dict_watcher_cb);
    if (g_dict_watcher_id < 0)
        return -1;
    g_dict_watcher_registered = 1;
    #endif
#endif
    return 0;
}

static void dict_iter_module_cleanup(void) {
#if PY_VERSION_HEX >= PY_VERSION_3_14_HEX
    #ifndef Py_GIL_DISABLED
    if (g_dict_watcher_registered) {
        PyDict_ClearWatcher(g_dict_watcher_id);
        g_dict_watcher_registered = 0;
        g_dict_watcher_id = -1;
    }
    #endif
#endif
}
#endif  // _COPIUM_DICT_ITER_C
