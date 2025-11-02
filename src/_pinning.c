/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * duper._pinning (compiled into copium._copying extension)
 * - Pin/PinsProxy types
 * - Open-addressed pin table
 * - Python APIs: pin / unpin / pinned / clear_pins / get_pins
 * - C hooks exported to _copying.c:
 *     PinObject* _duper_lookup_pin_for_object(PyObject* obj);
 *     int _duper_pinning_add_types(PyObject* module);
 */
#define PY_SSIZE_T_CLEAN

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "Python.h"

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

/* ------------------------------ Pin type ---------------------------------- */

typedef struct {
    PyObject_HEAD PyObject* snapshot; /* duper.snapshots.Snapshot */
    PyObject* factory;                /* callable reconstruct() */
    uint64_t hits;                    /* native counter */
} PinObject;

static PyTypeObject Pin_Type;

static PyObject* Pin_get_snapshot(PinObject* self, void* closure) {
    (void)closure;
    if (!self->snapshot)
        Py_RETURN_NONE;
    Py_INCREF(self->snapshot);
    return self->snapshot;
}
static PyObject* Pin_get_hits(PinObject* self, void* closure) {
    (void)closure;
    return PyLong_FromUnsignedLongLong(self->hits);
}
static PyObject* Pin_repr(PinObject* self) {
    PyObject* hits = PyLong_FromUnsignedLongLong(self->hits);
    if (!hits)
        return NULL;
    PyObject* repr_str = PyUnicode_FromFormat(
        "Pin(snapshot=%R, hits=%R)", self->snapshot ? self->snapshot : Py_None, hits
    );
    Py_DECREF(hits);
    return repr_str;
}
static void Pin_dealloc(PinObject* self) {
    Py_XDECREF(self->snapshot);
    Py_XDECREF(self->factory);
    Py_TYPE(self)->tp_free((PyObject*)self);
}
static PyGetSetDef Pin_getset[] = {
    {"snapshot", (getter)Pin_get_snapshot, NULL, "Snapshot object", NULL},
    {"hits", (getter)Pin_get_hits, NULL, "Reproduction hits (int)", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};
static PyTypeObject Pin_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium._copying.Pin",
    .tp_basicsize = sizeof(PinObject),
    .tp_dealloc = (destructor)Pin_dealloc,
    .tp_repr = (reprfunc)Pin_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc = "Pin(snapshot: Snapshot, hits: int)",
    .tp_getset = Pin_getset,
};

/* ------------------------------- Pin table -------------------------------- */

typedef struct {
    void* key;      /* object address; NULL = empty; (void*)-1 = tombstone */
    PinObject* pin; /* owned reference */
} PinEntry;

typedef struct {
    PinEntry* slots;
    Py_ssize_t size;   /* power of two */
    Py_ssize_t used;   /* non-empty (excludes tombstones) */
    Py_ssize_t filled; /* non-empty + tombstones */
} PinTable;

#define PIN_TOMBSTONE ((void*)(uintptr_t)(-1))

static PinTable* global_pin_table = NULL; /* owned here */


static void pin_table_free(PinTable* table) {
    if (!table)
        return;
    for (Py_ssize_t i = 0; i < table->size; i++) {
        if (table->slots[i].key && table->slots[i].key != PIN_TOMBSTONE) {
            Py_XDECREF(table->slots[i].pin);
        }
    }
    free(table->slots);
    free(table);
}

static int pin_table_resize(PinTable** table_ptr, Py_ssize_t min_capacity_needed) {
    PinTable* old = *table_ptr;
    Py_ssize_t new_size = 8;
    while (new_size < (min_capacity_needed * 2)) {
        new_size <<= 1;
        if (new_size <= 0) {
            new_size = (Py_ssize_t)1 << (sizeof(void*) * 8 - 2);
            break;
        }
    }
    PinEntry* new_slots = (PinEntry*)calloc((size_t)new_size, sizeof(PinEntry));
    if (!new_slots)
        return -1;

    PinTable* nt = (PinTable*)malloc(sizeof(PinTable));
    if (!nt) {
        free(new_slots);
        return -1;
    }
    nt->slots = new_slots;
    nt->size = new_size;
    nt->used = 0;
    nt->filled = 0;

    if (old) {
        for (Py_ssize_t i = 0; i < old->size; i++) {
            void* key = old->slots[i].key;
            if (key && key != PIN_TOMBSTONE) {
                PinObject* pin = old->slots[i].pin; /* transfer */
                Py_ssize_t mask = nt->size - 1;
                Py_ssize_t idx = hash_pointer(key) & mask;
                while (nt->slots[idx].key)
                    idx = (idx + 1) & mask;
                nt->slots[idx].key = key;
                nt->slots[idx].pin = pin;
                nt->used++;
                nt->filled++;
            }
        }
        free(old->slots);
        free(old);
    }
    *table_ptr = nt;
    return 0;
}

static inline int pin_table_ensure(PinTable** table_ptr) {
    if (*table_ptr)
        return 0;
    return pin_table_resize(table_ptr, 1);
}

static inline PinObject* pin_table_lookup(PinTable* table, void* key) {
    if (!table)
        return NULL;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash_pointer(key) & mask;
    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key)
            return NULL;
        if (slot_key != PIN_TOMBSTONE && slot_key == key) {
            return table->slots[idx].pin; /* borrowed */
        }
        idx = (idx + 1) & mask;
    }
}

static int pin_table_insert(PinTable** table_ptr, void* key, PinObject* pin) {
    if (pin_table_ensure(table_ptr) < 0)
        return -1;
    PinTable* table = *table_ptr;

    if ((table->filled * 10) >= (table->size * 7)) {
        if (pin_table_resize(table_ptr, table->used + 1) < 0)
            return -1;
        table = *table_ptr;
    }

    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash_pointer(key) & mask;
    Py_ssize_t first_tomb = -1;

    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key) {
            Py_ssize_t insert_at = (first_tomb >= 0) ? first_tomb : idx;
            table->slots[insert_at].key = key;
            Py_INCREF(pin);
            table->slots[insert_at].pin = pin;
            table->used++;
            table->filled++;
            return 0;
        }
        if (slot_key == PIN_TOMBSTONE) {
            if (first_tomb < 0)
                first_tomb = idx;
        } else if (slot_key == key) {
            Py_SETREF(table->slots[idx].pin, (PinObject*)Py_NewRef(pin));
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

static int pin_table_remove(PinTable* table, void* key) {
    if (!table)
        return -1;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash_pointer(key) & mask;
    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key)
            return -1; /* not found */
        if (slot_key != PIN_TOMBSTONE && slot_key == key) {
            table->slots[idx].key = PIN_TOMBSTONE;
            Py_XDECREF(table->slots[idx].pin);
            table->slots[idx].pin = NULL;
            table->used--;
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

/* ------------------------- PinsProxy & live views --------------------------
 */

typedef struct {
    PyObject_HEAD PinTable** table_ref; /* &global_pin_table */
} PinsProxy;

static PyTypeObject PinsProxy_Type;

typedef enum {
    PINS_IT_KEYS = 0,
    PINS_IT_VALUES = 1,
    PINS_IT_ITEMS = 2
} PinsIterKind;

typedef struct {
    PyObject_HEAD PinTable** table_ref;
} PinsView;
typedef struct {
    PyObject_HEAD PinTable** table_ref;
    PinsIterKind kind;
    Py_ssize_t index;
    PinTable* seen_table;
} PinsViewIter;

static PyTypeObject PinsKeysView_Type;
static PyTypeObject PinsValuesView_Type;
static PyTypeObject PinsItemsView_Type;
static PyTypeObject PinsViewIter_Type;

static inline PinTable* pinsproxy_get_table(PinsProxy* self) {
    return self->table_ref ? *self->table_ref : NULL;
}
static inline PinTable* pinsview_get_table(PinsView* self) {
    return self->table_ref ? *self->table_ref : NULL;
}
static inline PinTable* pinsiter_get_current_table(PinsViewIter* self) {
    return self->table_ref ? *self->table_ref : NULL;
}

static Py_ssize_t PinsProxy_len(PinsProxy* self) {
    PinTable* table = pinsproxy_get_table(self);
    return table ? table->used : 0;
}
static PyObject* PinsProxy_subscript(PinsProxy* self, PyObject* key) {
    if (!PyLong_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "keys are int id(obj)");
        return NULL;
    }
    void* ptr = PyLong_AsVoidPtr(key);
    if (ptr == NULL && PyErr_Occurred())
        return NULL;

    PinTable* table = pinsproxy_get_table(self);
    if (!table)
        goto not_found;

    {
        PinObject* pin = pin_table_lookup(table, ptr);
        if (pin) {
            Py_INCREF(pin);
            return (PyObject*)pin;
        }
    }
not_found:
    PyErr_SetObject(PyExc_KeyError, key);
    return NULL;
}
static PyObject* PinsProxy_iter(PinsProxy* self) {
    PinTable* table = pinsproxy_get_table(self);
    PyObject* keys_list = PyList_New(0);
    if (!keys_list)
        return NULL;
    if (table) {
        for (Py_ssize_t i = 0; i < table->size; i++) {
            if (table->slots[i].key && table->slots[i].key != PIN_TOMBSTONE) {
                PyObject* key_obj = PyLong_FromVoidPtr(table->slots[i].key);
                if (!key_obj || PyList_Append(keys_list, key_obj) < 0) {
                    Py_XDECREF(key_obj);
                    Py_DECREF(keys_list);
                    return NULL;
                }
                Py_DECREF(key_obj);
            }
        }
    }
    PyObject* it = PyObject_GetIter(keys_list);
    Py_DECREF(keys_list);
    return it;
}
static PyObject* pins_keysview_create(PinTable** table_ref);
static PyObject* pins_valuesview_create(PinTable** table_ref);
static PyObject* pins_itemsview_create(PinTable** table_ref);

static PyObject* PinsProxy_items(PinsProxy* self, PyObject* noargs) {
    (void)noargs;
    return pins_itemsview_create(self->table_ref);
}
static PyObject* PinsProxy_values(PinsProxy* self, PyObject* noargs) {
    (void)noargs;
    return pins_valuesview_create(self->table_ref);
}
static PyObject* PinsProxy_keys(PinsProxy* self, PyObject* noargs) {
    (void)noargs;
    return pins_keysview_create(self->table_ref);
}

static PyObject* PinsProxy_get(
    PinsProxy* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
) {
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        PyErr_SetString(PyExc_TypeError, "get() takes no keyword arguments");
        return NULL;
    }
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "get(key[, default])");
        return NULL;
    }
    PyObject* key = args[0];
    if (!PyLong_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "key must be int id(obj)");
        return NULL;
    }
    void* ptr = PyLong_AsVoidPtr(key);
    if (ptr == NULL && PyErr_Occurred())
        return NULL;

    PinTable* table = pinsproxy_get_table(self);
    PinObject* pin = table ? pin_table_lookup(table, ptr) : NULL;
    if (pin) {
        Py_INCREF(pin);
        return (PyObject*)pin;
    }

    if (nargs == 2) {
        PyObject* def = args[1];
        Py_INCREF(def);
        return def;
    }
    Py_RETURN_NONE;
}

static PyMappingMethods PinsProxy_as_mapping = {
    (lenfunc)PinsProxy_len, (binaryfunc)PinsProxy_subscript, 0
};
static PyMethodDef PinsProxy_methods[] = {
    {"items", (PyCFunction)PinsProxy_items, METH_NOARGS, "Return list-like live view of (id, Pin)"},
    {"values", (PyCFunction)PinsProxy_values, METH_NOARGS, "Return live view of Pin objects"},
    {"keys", (PyCFunction)PinsProxy_keys, METH_NOARGS, "Return live view of ids"},
    {"get",
     (PyCFunction)PinsProxy_get,
     METH_FASTCALL | METH_KEYWORDS,
     "get(key[, default]) -> Pin | default | None"},
    {NULL, NULL, 0, NULL}
};
static PyTypeObject PinsProxy_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium._copying.PinsProxy",
    .tp_basicsize = sizeof(PinsProxy),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Read-only live Mapping[int, Pin] over pin table",
    .tp_as_mapping = &PinsProxy_as_mapping,
    .tp_iter = (getiterfunc)PinsProxy_iter,
    .tp_methods = PinsProxy_methods,
};

static PyTypeObject PinsViewIter_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium._copying._pinsview_iterator",
    .tp_basicsize = sizeof(PinsViewIter),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "iterator over pin table",
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)NULL, /* set below */
};
static PyTypeObject PinsKeysView_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "dict_keys",
    .tp_basicsize = sizeof(PinsView),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "dict_keys",
    .tp_iter = (getiterfunc)NULL, /* set below */
    .tp_as_sequence = &(PySequenceMethods){.sq_length = (lenfunc)NULL} /* set later */,
};
static PyTypeObject PinsValuesView_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "dict_values",
    .tp_basicsize = sizeof(PinsView),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "dict_values",
    .tp_iter = (getiterfunc)NULL,
    .tp_as_sequence = &(PySequenceMethods){.sq_length = (lenfunc)NULL},
};
static PyTypeObject PinsItemsView_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "dict_items",
    .tp_basicsize = sizeof(PinsView),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "dict_items",
    .tp_iter = (getiterfunc)NULL,
    .tp_as_sequence = &(PySequenceMethods){.sq_length = (lenfunc)NULL},
};

/* Implementations filled after type objects exist */
static PyObject* PinsViewIter_iternext(PinsViewIter* iterator) {
    for (;;) {
        PinTable* current = pinsiter_get_current_table(iterator);
        if (!current)
            return NULL;

        if (current != iterator->seen_table) {
            iterator->seen_table = current;
            iterator->index = 0;
        }

        while (iterator->index < current->size) {
            Py_ssize_t slot_index = iterator->index++;

            PinTable* check = pinsiter_get_current_table(iterator);
            if (check != current)
                break; /* table swapped; restart outer loop */

            void* slot_key = current->slots[slot_index].key;
            if (!slot_key || slot_key == PIN_TOMBSTONE)
                continue;

            if (iterator->kind == PINS_IT_KEYS) {
                return PyLong_FromVoidPtr(slot_key);
            } else if (iterator->kind == PINS_IT_VALUES) {
                PinObject* pin = current->slots[slot_index].pin;
                Py_INCREF(pin);
                return (PyObject*)pin;
            } else {
                PyObject* key_obj = PyLong_FromVoidPtr(slot_key);
                if (!key_obj)
                    return NULL;
                PinObject* pin = current->slots[slot_index].pin;
                Py_INCREF(pin);
                PyObject* pair = PyTuple_New(2);
                if (!pair) {
                    Py_DECREF(key_obj);
                    Py_DECREF(pin);
                    return NULL;
                }
                PyTuple_SET_ITEM(pair, 0, key_obj);
                PyTuple_SET_ITEM(pair, 1, (PyObject*)pin);
                return pair;
            }
        }

        PinTable* after = pinsiter_get_current_table(iterator);
        if (!after)
            return NULL;
        if (after == iterator->seen_table && iterator->index >= after->size)
            return NULL;
    }
}

static PyObject* PinsView_iter_for_kind(PinsView* view, PinsIterKind kind) {
    PinsViewIter* it = PyObject_New(PinsViewIter, &PinsViewIter_Type);
    if (!it)
        return NULL;
    it->table_ref = view->table_ref;
    it->kind = kind;
    it->index = 0;
    it->seen_table = view->table_ref ? *view->table_ref : NULL;
    return (PyObject*)it;
}
static Py_ssize_t PinsView_len(PinsView* self) {
    PinTable* table = pinsview_get_table(self);
    return table ? table->used : 0;
}
static PyObject* PinsKeysView_iter(PyObject* self) {
    return PinsView_iter_for_kind((PinsView*)self, PINS_IT_KEYS);
}
static PyObject* PinsValuesView_iter(PyObject* self) {
    return PinsView_iter_for_kind((PinsView*)self, PINS_IT_VALUES);
}
static PyObject* PinsItemsView_iter(PyObject* self) {
    return PinsView_iter_for_kind((PinsView*)self, PINS_IT_ITEMS);
}

static PySequenceMethods PinsView_as_sequence = {.sq_length = (lenfunc)PinsView_len};

static PyObject* pins_keysview_create(PinTable** table_ref) {
    PinsView* v = PyObject_New(PinsView, &PinsKeysView_Type);
    if (!v)
        return NULL;
    v->table_ref = table_ref;
    return (PyObject*)v;
}
static PyObject* pins_valuesview_create(PinTable** table_ref) {
    PinsView* v = PyObject_New(PinsView, &PinsValuesView_Type);
    if (!v)
        return NULL;
    v->table_ref = table_ref;
    return (PyObject*)v;
}
static PyObject* pins_itemsview_create(PinTable** table_ref) {
    PinsView* v = PyObject_New(PinsView, &PinsItemsView_Type);
    if (!v)
        return NULL;
    v->table_ref = table_ref;
    return (PyObject*)v;
}

/* Factory for PinsProxy bound to &global_pin_table */
static PyObject* PinsProxy_create_bound_to_global(void) {
    PinsProxy* proxy = PyObject_New(PinsProxy, &PinsProxy_Type);
    if (!proxy)
        return NULL;
    proxy->table_ref = &global_pin_table;
    return (PyObject*)proxy;
}

/* --------------------------- Snapshot integration --------------------------
 */
/* Local cache of Snapshot and constructor. This avoids depending on _copying.c
 * state. */
typedef struct {
    PyObject* snapshots_mod;
    PyObject* Snapshot_type;
    PyObject* Snapshot_take;
} SnapshotCache;
static SnapshotCache snapshot_cache = {0};

static int ensure_snapshot_cache(void) {
    if (snapshot_cache.Snapshot_type && snapshot_cache.Snapshot_take)
        return 0;

    PyObject* snapshots = PyImport_ImportModule("duper.snapshots");
    if (!snapshots)
        return -1;
    snapshot_cache.snapshots_mod = snapshots; /* keep ref */

    PyObject* Snapshot = PyObject_GetAttrString(snapshots, "Snapshot");
    if (!Snapshot || !PyType_Check(Snapshot)) {
        Py_XDECREF(Snapshot);
        return -1;
    }
    snapshot_cache.Snapshot_type = Snapshot;

    PyObject* take = PyObject_GetAttrString(Snapshot, "take");
    if (!take)
        return -1;
    snapshot_cache.Snapshot_take = take;
    return 0;
}

static PinObject* create_pin_for_object(PyObject* obj) {
    if (UNLIKELY(ensure_snapshot_cache() < 0))
        return NULL;

    PyObject* snapshot = PyObject_CallOneArg(snapshot_cache.Snapshot_take, obj);
    if (!snapshot)
        return NULL;

    PyObject* factory = PyObject_GetAttrString(snapshot, "reconstruct");
    if (!factory || !PyCallable_Check(factory)) {
        Py_DECREF(snapshot);
        Py_XDECREF(factory);
        return NULL;
    }

    PinObject* pin = PyObject_New(PinObject, &Pin_Type);
    if (!pin) {
        Py_DECREF(snapshot);
        Py_DECREF(factory);
        return NULL;
    }
    pin->snapshot = snapshot;
    pin->factory = factory;
    pin->hits = 0;
    return pin;
}

/* ------------------------ C hooks exported to _copying.c -------------------
 */
PinObject* _duper_lookup_pin_for_object(PyObject* obj) {
    if (!global_pin_table)
        return NULL;
    return pin_table_lookup(global_pin_table, (void*)obj);
}

/* ------------------------- Python-callable functions -----------------------
 */

PyObject* py_pin(PyObject* self, PyObject* obj) {
    (void)self;
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "pin(obj) missing obj");
        return NULL;
    }
    PinObject* pin = create_pin_for_object(obj);
    if (!pin)
        return NULL;
    if (pin_table_insert(&global_pin_table, (void*)obj, pin) < 0) {
        Py_DECREF(pin);
        PyErr_SetString(PyExc_RuntimeError, "pin: failed to store Pin");
        return NULL;
    }
    return (PyObject*)pin;
}

PyObject* py_unpin(PyObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames) {
    (void)self;
    if (UNLIKELY(nargs < 1)) {
        PyErr_SetString(PyExc_TypeError, "unpin() missing 1 required positional argument: 'obj'");
        return NULL;
    }
    PyObject* obj = args[0];
    int strict_mode = 0;

    if (kwnames) {
        Py_ssize_t keyword_count = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < keyword_count; i++) {
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, i);
            if (!PyUnicode_Check(kwname)) {
                PyErr_SetString(PyExc_TypeError, "keyword name must be str");
                return NULL;
            }
            int is_strict = PyUnicode_CompareWithASCIIString(kwname, "strict") == 0;
            if (!is_strict) {
                PyErr_SetString(PyExc_TypeError, "unpin() got an unexpected keyword argument");
                return NULL;
            }
            PyObject* kwvalue = args[nargs + i];
            int truthy = PyObject_IsTrue(kwvalue);
            if (truthy < 0)
                return NULL;
            strict_mode = truthy;
        }
    }

    if (!global_pin_table) {
        if (strict_mode) {
            PyErr_SetString(PyExc_KeyError, "object not pinned");
            return NULL;
        }
        Py_RETURN_NONE;
    }

    if (pin_table_remove(global_pin_table, (void*)obj) < 0) {
        if (strict_mode) {
            PyErr_SetString(PyExc_KeyError, "object not pinned");
            return NULL;
        }
        PyErr_Clear();
    }

    if (global_pin_table->used == 0) {
        pin_table_free(global_pin_table);
        global_pin_table = NULL;
    }
    Py_RETURN_NONE;
}

PyObject* py_pinned(PyObject* self, PyObject* obj) {
    (void)self;
    if (!obj) {
        PyErr_SetString(PyExc_TypeError, "pinned(obj) missing obj");
        return NULL;
    }
    PinObject* pin = _duper_lookup_pin_for_object(obj);
    if (!pin)
        Py_RETURN_NONE;
    Py_INCREF(pin);
    return (PyObject*)pin;
}

PyObject* py_clear_pins(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;
    if (global_pin_table) {
        pin_table_free(global_pin_table);
        global_pin_table = NULL;
    }
    Py_RETURN_NONE;
}

PyObject* py_get_pins(PyObject* self, PyObject* noargs) {
    (void)self;
    (void)noargs;
    return PinsProxy_create_bound_to_global();
}

/* ------------------ Registration with collections.abc ----------------------
 */

static int register_type_with_abc(PyObject* abc_type, PyObject* concrete_type) {
    PyObject* register_method = PyObject_GetAttrString(abc_type, "register");
    if (!register_method)
        return -1;
    PyObject* res = PyObject_CallOneArg(register_method, concrete_type);
    Py_DECREF(register_method);
    if (!res)
        return -1;
    Py_DECREF(res);
    return 0;
}

/* -------------- Public function: add types to module on init ---------------
 */
int _duper_pinning_add_types(PyObject* module) {
    if (PyType_Ready(&Pin_Type) < 0)
        return -1;
    if (PyType_Ready(&PinsProxy_Type) < 0)
        return -1;

    /* Views require method pointers set now */
    PinsViewIter_Type.tp_iternext = (iternextfunc)PinsViewIter_iternext;
    PinsKeysView_Type.tp_iter = (getiterfunc)PinsKeysView_iter;
    PinsValuesView_Type.tp_iter = (getiterfunc)PinsValuesView_iter;
    PinsItemsView_Type.tp_iter = (getiterfunc)PinsItemsView_iter;
    PinsKeysView_Type.tp_as_sequence = &PinsView_as_sequence;
    PinsValuesView_Type.tp_as_sequence = &PinsView_as_sequence;
    PinsItemsView_Type.tp_as_sequence = &PinsView_as_sequence;
    if (PyType_Ready(&PinsViewIter_Type) < 0)
        return -1;
    if (PyType_Ready(&PinsKeysView_Type) < 0)
        return -1;
    if (PyType_Ready(&PinsValuesView_Type) < 0)
        return -1;
    if (PyType_Ready(&PinsItemsView_Type) < 0)
        return -1;

    Py_INCREF(&Pin_Type);
    if (PyModule_AddObject(module, "Pin", (PyObject*)&Pin_Type) < 0) {
        Py_DECREF(&Pin_Type);
        return -1;
    }
    Py_INCREF(&PinsProxy_Type);
    if (PyModule_AddObject(module, "PinsProxy", (PyObject*)&PinsProxy_Type) < 0) {
        Py_DECREF(&PinsProxy_Type);
        return -1;
    }

    /* Register with collections.abc ABCs */
    PyObject* mod_abc = PyImport_ImportModule("collections.abc");
    if (!mod_abc)
        return -1;
    PyObject* Mapping = PyObject_GetAttrString(mod_abc, "Mapping");
    PyObject* KeysView = PyObject_GetAttrString(mod_abc, "KeysView");
    PyObject* ValuesView = PyObject_GetAttrString(mod_abc, "ValuesView");
    PyObject* ItemsView = PyObject_GetAttrString(mod_abc, "ItemsView");
    int ok = 0;
    if (!Mapping || !KeysView || !ValuesView || !ItemsView)
        ok = -1;
    if (!ok && register_type_with_abc(Mapping, (PyObject*)&PinsProxy_Type) < 0)
        ok = -1;
    if (!ok && register_type_with_abc(KeysView, (PyObject*)&PinsKeysView_Type) < 0)
        ok = -1;
    if (!ok && register_type_with_abc(ValuesView, (PyObject*)&PinsValuesView_Type) < 0)
        ok = -1;
    if (!ok && register_type_with_abc(ItemsView, (PyObject*)&PinsItemsView_Type) < 0)
        ok = -1;

    Py_XDECREF(Mapping);
    Py_XDECREF(KeysView);
    Py_XDECREF(ValuesView);
    Py_XDECREF(ItemsView);
    Py_DECREF(mod_abc);

    return ok ? -1 : 0;
}