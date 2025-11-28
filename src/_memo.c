/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * copium._memo (compiled into copium._copying extension)
 * - Memo type (internal hash table for deepcopy memo)
 * - Implements MutableMapping protocol with views
 * - Keepalive vector with a Python-facing proxy implementing a MutableSequence
 */
#ifndef _COPIUM_MEMO_C
#define _COPIUM_MEMO_C

#include "copium_common.h"
#include "_state.c"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------ Memo table -------------------------------- */

typedef struct {
    void* key;
    PyObject* value; /* stored with a strong reference inside the table */
} MemoEntry;

typedef struct {
    MemoEntry* slots;
    Py_ssize_t size;   /* power-of-two capacity */
    Py_ssize_t used;   /* number of live entries */
    Py_ssize_t filled; /* live + tombstones */
} MemoTable;

typedef struct {
    PyObject** items;
    Py_ssize_t size;
    Py_ssize_t capacity;
} KeepaliveVector;

/* Exact runtime layout of the memo object (must begin with PyObject_HEAD). */
typedef struct _MemoObject {
    PyObject_HEAD MemoTable* table;
    KeepaliveVector keepalive;
} MemoObject;

/* Forward decl to refer to Memo_Type in helpers */
typedef struct _MemoObject MemoObject;

#define MEMO_TOMBSTONE ((void*)(uintptr_t)(-1))

/* SplitMix64-style pointer hasher, stable across the process. */
static ALWAYS_INLINE Py_ssize_t hash_pointer(void* ptr) {
    uintptr_t h = (uintptr_t)ptr;
    h ^= h >> 33;
    h *= (uintptr_t)0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (Py_ssize_t)h;
}

/* Exported so _copying.c can compute the exact same hash once per object */
static ALWAYS_INLINE Py_ssize_t memo_hash_pointer(void* ptr) {
    return hash_pointer(ptr);
}

/* Retention policy caps for TLS memo/keepalive reuse */
#ifndef COPIUM_MEMO_RETAIN_MAX_SLOTS
    #define COPIUM_MEMO_RETAIN_MAX_SLOTS (1 << 17) /* 131072 slots (~2 MiB for 16B entries) */
#endif
#ifndef COPIUM_MEMO_RETAIN_SHRINK_TO
    #define COPIUM_MEMO_RETAIN_SHRINK_TO (1 << 13) /* 8192 slots */
#endif
#ifndef COPIUM_KEEP_RETAIN_MAX
    #define COPIUM_KEEP_RETAIN_MAX (1 << 13) /* 8192 elements */
#endif
#ifndef COPIUM_KEEP_RETAIN_TARGET
    #define COPIUM_KEEP_RETAIN_TARGET (1 << 10) /* 1024 elements */
#endif

/* ------------------------------ Keep vector impl --------------------------- */

static void keepalive_init(KeepaliveVector* kv) {
    kv->items = NULL;
    kv->size = 0;
    kv->capacity = 0;
}

static int keepalive_grow(KeepaliveVector* kv, Py_ssize_t min_capacity) {
    Py_ssize_t new_cap = kv->capacity > 0 ? kv->capacity : 8;
    while (new_cap < min_capacity) {
        /* simple doubling with overflow clamp */
        Py_ssize_t next = new_cap << 1;
        if (next <= 0 || next < new_cap) { /* overflow */
            new_cap = min_capacity;
            break;
        }
        new_cap = next;
    }
    PyObject** ni = (PyObject**)PyMem_Realloc(kv->items, (size_t)new_cap * sizeof(PyObject*));
    if (!ni)
        return -1;
    kv->items = ni;
    kv->capacity = new_cap;
    return 0;
}

int keepalive_append(KeepaliveVector* kv, PyObject* obj) {
    if (kv->size >= kv->capacity) {
        if (keepalive_grow(kv, kv->size + 1) < 0)
            return -1;
    }
    Py_INCREF(obj);
    kv->items[kv->size++] = obj;
    return 0;
}

void keepalive_clear(KeepaliveVector* kv) {
    for (Py_ssize_t i = 0; i < kv->size; i++) {
        Py_XDECREF(kv->items[i]);
    }
    kv->size = 0;
}

static void keepalive_free(KeepaliveVector* kv) {
    keepalive_clear(kv);
    PyMem_Free(kv->items);
    kv->items = NULL;
    kv->capacity = 0;
}

/* Shrink capacity if it ballooned past the cap; keep it modest thereafter. */
void keepalive_shrink_if_large(KeepaliveVector* kv) {
    if (!kv || !kv->items)
        return;
    if (kv->capacity > COPIUM_KEEP_RETAIN_MAX) {
        Py_ssize_t target = COPIUM_KEEP_RETAIN_TARGET;
        if (target < kv->size) {
            /* In practice size==0 after clear, but be safe. */
            target = kv->size ? kv->size : 1;
        }
        PyObject** ni = (PyObject**)PyMem_Realloc(kv->items, (size_t)target * sizeof(PyObject*));
        if (ni) {
            kv->items = ni;
            kv->capacity = target;
        }
        /* If realloc fails, we keep the larger buffer; correctness preserved. */
    }
}

/* ------------------------------ Memo table impl ---------------------------- */

void memo_table_free(MemoTable* table) {
    if (!table)
        return;
    for (Py_ssize_t i = 0; i < table->size; i++) {
        if (table->slots[i].key && table->slots[i].key != MEMO_TOMBSTONE) {
            Py_XDECREF(table->slots[i].value);
        }
    }
    free(table->slots);
    free(table);
}

/* Forward decls: keep non-static clear visible cross-file; resize is local static. */
void memo_table_clear(MemoTable* table);
static int memo_table_resize(MemoTable** table_ptr, Py_ssize_t min_capacity_needed);

/* Reset-with-policy: clear, and shrink if past cap back to a small target. */
int memo_table_reset(MemoTable** table_ptr) {
    if (!table_ptr)
        return 0;
    MemoTable* t = *table_ptr;
    if (!t)
        return 0;

    /* Always drop references and zero slots first. */
    memo_table_clear(t);

    if (t->size > COPIUM_MEMO_RETAIN_MAX_SLOTS) {
        /* Rebuild a smaller table; migrating 0 entries is cheap. */
        Py_ssize_t min_needed = COPIUM_MEMO_RETAIN_SHRINK_TO / 2;
        if (min_needed < 1)
            min_needed = 1;
        if (memo_table_resize(table_ptr, min_needed) < 0) {
            return -1;
        }
    }
    return 0;
}

/* New: reset table contents in-place but keep capacity for reuse (TLS buffer) */
void memo_table_clear(MemoTable* table) {
    if (!table)
        return;
    for (Py_ssize_t i = 0; i < table->size; i++) {
        void* key = table->slots[i].key;
        if (key && key != MEMO_TOMBSTONE) {
            Py_XDECREF(table->slots[i].value);
        }
        table->slots[i].key = NULL;
        table->slots[i].value = NULL;
    }
    table->used = 0;
    table->filled = 0;
}

static int memo_table_resize(MemoTable** table_ptr, Py_ssize_t min_capacity_needed) {
    MemoTable* old = *table_ptr;
    Py_ssize_t new_size = 8;
    while (new_size < (min_capacity_needed * 2)) {
        Py_ssize_t next = new_size << 1;
        if (next <= 0 || next < new_size) { /* overflow clamp */
            new_size = (Py_ssize_t)1 << (sizeof(void*) * 8 - 2);
            break;
        }
        new_size = next;
    }

    MemoEntry* new_slots = (MemoEntry*)calloc((size_t)new_size, sizeof(MemoEntry));
    if (!new_slots)
        return -1;

    MemoTable* nt = (MemoTable*)malloc(sizeof(MemoTable));
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
            if (key && key != MEMO_TOMBSTONE) {
                PyObject* value = old->slots[i].value; /* transfer */
                Py_ssize_t mask = nt->size - 1;
                Py_ssize_t idx = hash_pointer(key) & mask;
                while (nt->slots[idx].key)
                    idx = (idx + 1) & mask;
                nt->slots[idx].key = key;
                nt->slots[idx].value = value;
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

static ALWAYS_INLINE int memo_table_ensure(MemoTable** table_ptr) {
    if (*table_ptr)
        return 0;
    return memo_table_resize(table_ptr, 1);
}

/* Existing non-hash-parameterized APIs (kept for compatibility) */
static ALWAYS_INLINE PyObject* memo_table_lookup(MemoTable* table, void* key) {
    if (!table)
        return NULL;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash_pointer(key) & mask;
    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key)
            return NULL;
        if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
            return table->slots[idx].value; /* borrowed */
        }
        idx = (idx + 1) & mask;
    }
}

static ALWAYS_INLINE int memo_table_insert(MemoTable** table_ptr, void* key, PyObject* value) {
    if (memo_table_ensure(table_ptr) < 0)
        return -1;
    MemoTable* table = *table_ptr;

    if ((table->filled * 10) >= (table->size * 7)) {
        if (memo_table_resize(table_ptr, table->used + 1) < 0)
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
            Py_INCREF(value);
            table->slots[insert_at].value = value;
            table->used++;
            table->filled++;
            return 0;
        }
        if (slot_key == MEMO_TOMBSTONE) {
            if (first_tomb < 0)
                first_tomb = idx;
        } else if (slot_key == key) {
            PyObject* old_value = table->slots[idx].value;
            Py_INCREF(value);
            table->slots[idx].value = value;
            Py_XDECREF(old_value);
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

/* New hash-parameterized hot-path APIs (avoid recomputing hash) */
static ALWAYS_INLINE PyObject* memo_table_lookup_h(MemoTable* table, void* key, Py_ssize_t hash) {
    if (!table)
        return NULL;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash & mask;
    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key)
            return NULL;
        if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
            return table->slots[idx].value; /* borrowed */
        }
        idx = (idx + 1) & mask;
    }
}

static ALWAYS_INLINE int memo_table_insert_h(
    MemoTable** table_ptr, void* key, PyObject* value, Py_ssize_t hash
) {
    if (memo_table_ensure(table_ptr) < 0)
        return -1;
    MemoTable* table = *table_ptr;

    if ((table->filled * 10) >= (table->size * 7)) {
        if (memo_table_resize(table_ptr, table->used + 1) < 0)
            return -1;
        table = *table_ptr;
    }

    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash & mask;
    Py_ssize_t first_tomb = -1;

    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key) {
            Py_ssize_t insert_at = (first_tomb >= 0) ? first_tomb : idx;
            table->slots[insert_at].key = key;
            Py_INCREF(value);
            table->slots[insert_at].value = value;
            table->used++;
            table->filled++;
            return 0;
        }
        if (slot_key == MEMO_TOMBSTONE) {
            if (first_tomb < 0)
                first_tomb = idx;
        } else if (slot_key == key) {
            PyObject* old_value = table->slots[idx].value;
            Py_INCREF(value);
            table->slots[idx].value = value;
            Py_XDECREF(old_value);
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

int memo_table_remove(MemoTable* table, void* key) {
    if (!table)
        return -1;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash_pointer(key) & mask;
    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key)
            return -1; /* not found */
        if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
            table->slots[idx].key = MEMO_TOMBSTONE;
            Py_XDECREF(table->slots[idx].value);
            table->slots[idx].value = NULL;
            table->used--;
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

/* ------------------------- Memo type & keepalive proxy -------------------------- */

PyTypeObject Memo_Type;

/* _KeepaliveList proxy ============================================================
 * A thin Python object that forwards to MemoObject.keep.
 * It is *not* the owner of the storage; it keeps a strong ref to MemoObject.
 */

typedef struct {
    PyObject_HEAD MemoObject* owner; /* strong ref to the memo owning the vector */
} KeepaliveListObject;

static PyTypeObject KeepaliveList_Type;

/* Forward decls */
static PyObject* KeepaliveList_New(MemoObject* owner);

static void KeepaliveList_dealloc(KeepaliveListObject* self) {
    Py_XDECREF(self->owner);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static Py_ssize_t KeepaliveList_len(KeepaliveListObject* self) {
    return self->owner && self->owner->keepalive.items ? self->owner->keepalive.size : 0;
}

static PyObject* KeepaliveList_getitem(KeepaliveListObject* self, Py_ssize_t index) {
    if (!self->owner) {
        PyErr_SetString(PyExc_SystemError, "_KeepaliveList has no owner");
        return NULL;
    }
    KeepaliveVector* kv = &self->owner->keepalive;
    Py_ssize_t n = kv->size;
    if (index < 0)
        index += n;
    if (index < 0 || index >= n) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        return NULL;
    }
    PyObject* item = kv->items[index];
    return Py_NewRef(item);
}

static PyObject* KeepaliveList_iter(KeepaliveListObject* self) {
    if (!self->owner) {
        PyErr_SetString(PyExc_SystemError, "_KeepaliveList has no owner");
        return NULL;
    }
    KeepaliveVector* kv = &self->owner->keepalive;
    Py_ssize_t n = kv->size;
    PyObject* list = PyList_New(n);
    if (!list)
        return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* it = kv->items[i];
        Py_INCREF(it);
        PyList_SET_ITEM(list, i, it);
    }
    PyObject* it = PyObject_GetIter(list);
    Py_DECREF(list);
    return it;
}

static PyObject* KeepaliveList_append(KeepaliveListObject* self, PyObject* arg) {
    if (!self->owner) {
        PyErr_SetString(PyExc_SystemError, "_KeepaliveList has no owner");
        return NULL;
    }
    if (keepalive_append(&self->owner->keepalive, arg) < 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyObject* KeepaliveList_clear(KeepaliveListObject* self, PyObject* noargs) {
    (void)noargs;
    if (!self->owner) {
        PyErr_SetString(PyExc_SystemError, "_KeepaliveList has no owner");
        return NULL;
    }
    keepalive_clear(&self->owner->keepalive);
    Py_RETURN_NONE;
}

static PySequenceMethods KeepaliveList_as_sequence = {
    (lenfunc)KeepaliveList_len,          /* sq_length */
    0,                                   /* sq_concat */
    0,                                   /* sq_repeat */
    (ssizeargfunc)KeepaliveList_getitem, /* sq_item */
    0,                                   /* sq_slice (deprecated) */
    0,                                   /* sq_ass_item */
    0,                                   /* sq_ass_slice (deprecated) */
    0,                                   /* sq_contains */
    0,                                   /* sq_inplace_concat */
    0                                    /* sq_inplace_repeat */
};

static PyMethodDef KeepaliveList_methods[] = {
    {"append", (PyCFunction)KeepaliveList_append, METH_O, NULL},
    {"clear", (PyCFunction)KeepaliveList_clear, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyObject* KeepaliveList_New(MemoObject* owner) {
    KeepaliveListObject* self = PyObject_New(KeepaliveListObject, &KeepaliveList_Type);
    if (!self)
        return NULL;
    Py_INCREF(owner);
    self->owner = owner;
    return (PyObject*)self;
}

static PyTypeObject KeepaliveList_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium._KeepaliveList",
    .tp_basicsize = sizeof(KeepaliveListObject),
    .tp_dealloc = (destructor)KeepaliveList_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_as_sequence = &KeepaliveList_as_sequence,
    .tp_iter = (getiterfunc)KeepaliveList_iter,
    .tp_methods = KeepaliveList_methods,
};

/* --------------------------- Memo object impl ------------------------------ */

static void Memo_dealloc(MemoObject* self) {
    PyObject_GC_UnTrack(self);  // Stop tracking before destruction
    if (PyObject_CallFinalizerFromDealloc((PyObject*)self)) {
        return;
    }
    memo_table_free(self->table);
    keepalive_free(&self->keepalive);
    PyObject_GC_Del(self);  // Use GC-aware free
}

static int Memo_traverse(MemoObject* self, visitproc visit, void* arg) {
    if (self->table) {
        for (Py_ssize_t i = 0; i < self->table->size; i++) {
            void* key = self->table->slots[i].key;
            if (key && key != MEMO_TOMBSTONE) {
                Py_VISIT(self->table->slots[i].value);
            }
        }
    }
    for (Py_ssize_t i = 0; i < self->keepalive.size; i++) {
        Py_VISIT(self->keepalive.items[i]);
    }
    return 0;
}

static int Memo_clear_gc(MemoObject* self) {
    // Break all references
    if (self->table) {
        memo_table_clear(self->table);
    }
    keepalive_clear(&self->keepalive);
    return 0;
}

PyObject* Memo_New(void) {
    MemoObject* self = PyObject_GC_New(MemoObject, &Memo_Type);
    if (!self)
        return NULL;
    self->table = NULL;
    // Since memo is designed to be reused, unless stolen, don't call PyObject_GC_Track just yet.
    // Instead, call it once we know that somebody stole the ref.
    keepalive_init(&self->keepalive);
    return (PyObject*)self;
}

static Py_ssize_t Memo_len(MemoObject* self) {
    return self->table ? self->table->used : 0;
}

static PyObject* Memo_subscript(MemoObject* self, PyObject* pykey) {
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_KeyError, "keys must be integers");
        return NULL;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return NULL;

    /* Special-case for keepalive: memo[id(memo)] */
    if (key == (void*)self) {
        /* Return a fresh proxy bound to the internal keepalive vector.
       Do NOT store it in the table to avoid creating a non-GC-tracked cycle. */
        return KeepaliveList_New(self);
    }

    PyObject* value = memo_table_lookup(self->table, key);
    if (!value) {
        PyErr_SetObject(PyExc_KeyError, pykey);
        return NULL;
    }
    Py_INCREF(value);
    return value;
}

static int Memo_ass_subscript(MemoObject* self, PyObject* pykey, PyObject* value) {
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_KeyError, "keys must be integers");
        return -1;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return -1;

    if (value == NULL) {
        int res = memo_table_remove(self->table, key);
        if (res < 0) {
            PyErr_SetObject(PyExc_KeyError, pykey);
        }
        return res;
    } else {
        return memo_table_insert(&(self->table), key, value);
    }
}

static PyObject* Memo_iter(MemoObject* self) {
    PyObject* keys_list = PyList_New(0);
    if (!keys_list)
        return NULL;
    if (self->table) {
        for (Py_ssize_t i = 0; i < self->table->size; i++) {
            void* key = self->table->slots[i].key;
            if (key && key != MEMO_TOMBSTONE) {
                PyObject* py_key = PyLong_FromVoidPtr(key);
                if (!py_key || PyList_Append(keys_list, py_key) < 0) {
                    Py_XDECREF(py_key);
                    Py_DECREF(keys_list);
                    return NULL;
                }
                Py_DECREF(py_key);
            }
        }
    }
    PyObject* it = PyObject_GetIter(keys_list);
    Py_DECREF(keys_list);
    return it;
}

static PyObject* Memo_clear(MemoObject* self, PyObject* noargs) {
    (void)noargs;
    /* Keep the allocated table capacity for reuse; just clear contents. */
    if (self->table) {
        memo_table_clear(self->table);
    }
    /* Keep the keepalive vector capacity; only DECREF contained objects. */
    keepalive_clear(&self->keepalive);
    Py_RETURN_NONE;
}

/* __del__: drop strong references held by the memo so user-retained memos
   can clean up deterministically when collected. */
static PyObject* Memo___del__(MemoObject* self, PyObject* noargs) {
    (void)noargs;
    if (self->table) {
        memo_table_clear(self->table);
    }
    keepalive_clear(&self->keepalive);
    Py_RETURN_NONE;
}

static PyObject* Memo_contains(MemoObject* self, PyObject* pykey) {
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_KeyError, "keys must be integers");
        return NULL;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return NULL;
    PyObject* value = memo_table_lookup(self->table, key);
    return PyBool_FromLong(value != NULL);
}

static PyObject* Memo_keep(MemoObject* self, PyObject* noargs) {
    (void)noargs;
    /* Expose a (fresh) proxy each time; storage lives in self->keepalive. */
    return KeepaliveList_New(self);
}

static PyMappingMethods Memo_as_mapping = {
    (lenfunc)Memo_len, (binaryfunc)Memo_subscript, (objobjargproc)Memo_ass_subscript
};

/* Late-bound 'get' to keep identical signature as before */
static PyObject* Memo_get(MemoObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "get expected 1 or 2 arguments");
        return NULL;
    }
    PyObject* pykey = args[0];
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_KeyError, "keys must be integers");
        return NULL;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return NULL;
    PyObject* value = memo_table_lookup(self->table, key);
    if (value) {
        Py_INCREF(value);
        return value;
    } else {
        if (nargs == 2) {
            Py_INCREF(args[1]);
            return args[1];
        } else {
            PyErr_SetObject(PyExc_KeyError, pykey);
            return NULL;
        }
    }
}

static PyObject* Memo_setdefault(MemoObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "setdefault expected 1 or 2 arguments");
        return NULL;
    }
    PyObject* pykey = args[0];
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_KeyError, "keys must be integers");
        return NULL;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return NULL;

    /* Special handling: id(memo) exposes keepalive proxy without storing. */
    if (key == (void*)self) {
        return KeepaliveList_New(self);
    }

    /* Existing value? */
    PyObject* value = memo_table_lookup(self->table, key);
    if (value) {
        Py_INCREF(value);
        return value;
    }

    /* No existing value: store default (or None if omitted) and return it. */
    PyObject* def = (nargs == 2) ? args[1] : Py_None;
    if (memo_table_insert(&self->table, key, def) < 0) {
        return NULL;
    }
    Py_INCREF(def);
    return def;
}

static PyMethodDef Memo_methods[] = {
    {"clear", (PyCFunction)Memo_clear, METH_NOARGS, NULL},
    {"get", (PyCFunction)NULL, METH_FASTCALL, NULL}, /* populated below for ABI stability */
    {"setdefault", (PyCFunction)Memo_setdefault, METH_FASTCALL, NULL},
    {"__contains__", (PyCFunction)Memo_contains, METH_O, NULL},
    {"keep", (PyCFunction)Memo_keep, METH_NOARGS, NULL}, /* expose keepalive proxy */
    {"__del__", (PyCFunction)Memo___del__, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL}
};

PyTypeObject Memo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium._Memo",
    .tp_basicsize = sizeof(MemoObject),
    .tp_dealloc = (destructor)Memo_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_as_mapping = &Memo_as_mapping,
    .tp_iter = (getiterfunc)Memo_iter,
    .tp_methods = Memo_methods,
    .tp_traverse = (traverseproc)Memo_traverse,
    .tp_clear = (inquiry)Memo_clear_gc,
};

/* --------------------------- Type readiness helper ------------------------- */

/* Called from module init in _copying.c */
int memo_ready_types(void) {
    /* Fill in Memo.get at runtime (keeps the .methods table layout stable) */
    for (PyMethodDef* m = Memo_methods; m && m->ml_name; ++m) {
        if (m->ml_meth == NULL && strcmp(m->ml_name, "get") == 0) {
            m->ml_meth = (PyCFunction)Memo_get;
            m->ml_flags = METH_FASTCALL;
            break;
        }
    }

    if (PyType_Ready(&KeepaliveList_Type) < 0)
        return -1;
    if (PyType_Ready(&Memo_Type) < 0)
        return -1;
    return 0;
}
#endif  // _COPIUM_MEMO_C



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