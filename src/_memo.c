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

#include "_common.h"
#include "_state.c"
#include "_abc_registration.c"

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

/* Undo log for rollback support - tracks keys inserted since last checkpoint */
typedef struct {
    void** keys;
    Py_ssize_t size;
    Py_ssize_t capacity;
} MemoUndoLog;

/* Exact runtime layout of the memo object (must begin with PyObject_HEAD). */
typedef struct _PyMemoObject {
    PyObject_HEAD MemoTable* table;
    KeepaliveVector keepalive;
    MemoUndoLog undo_log;
} PyMemoObject;

/* Forward decl to refer to Memo_Type in helpers */
typedef struct _PyMemoObject PyMemoObject;

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

/* ------------------------------ Undo log impl ------------------------------ */

static void undo_log_init(MemoUndoLog* log) {
    log->keys = NULL;
    log->size = 0;
    log->capacity = 0;
}

static int undo_log_append(MemoUndoLog* log, void* key) {
    if (log->size >= log->capacity) {
        Py_ssize_t new_cap = log->capacity > 0 ? log->capacity * 2 : 16;
        void** new_keys = (void**)PyMem_Realloc(log->keys, (size_t)new_cap * sizeof(void*));
        if (!new_keys)
            return -1;
        log->keys = new_keys;
        log->capacity = new_cap;
    }
    log->keys[log->size++] = key;
    return 0;
}

static void undo_log_clear(MemoUndoLog* log) {
    log->size = 0;
    /* Keep allocated buffer for reuse */
}

static void undo_log_free(MemoUndoLog* log) {
    PyMem_Free(log->keys);
    log->keys = NULL;
    log->size = 0;
    log->capacity = 0;
}

/* Shrink undo log capacity if it ballooned past a threshold */
static void undo_log_shrink_if_large(MemoUndoLog* log) {
    if (!log || !log->keys)
        return;
    /* Use same thresholds as keepalive */
    if (log->capacity > COPIUM_KEEP_RETAIN_MAX) {
        Py_ssize_t target = COPIUM_KEEP_RETAIN_TARGET;
        if (target < log->size)
            target = log->size ? log->size : 1;
        void** ni = (void**)PyMem_Realloc(log->keys, (size_t)target * sizeof(void*));
        if (ni) {
            log->keys = ni;
            log->capacity = target;
        }
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

static ALWAYS_INLINE int memo_table_remove_h(MemoTable* table, void* key, Py_ssize_t hash) {
    if (!table)
        return -1;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash & mask;
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

int memo_table_remove(MemoTable* table, void* key) {
    return memo_table_remove_h(table, key, hash_pointer(key));
}

/* Remove and return value (for pop). Returns borrowed ref to value before removal. */
static PyObject* memo_table_pop(MemoTable* table, void* key) {
    if (!table)
        return NULL;
    Py_ssize_t mask = table->size - 1;
    Py_ssize_t idx = hash_pointer(key) & mask;
    for (;;) {
        void* slot_key = table->slots[idx].key;
        if (!slot_key)
            return NULL; /* not found */
        if (slot_key != MEMO_TOMBSTONE && slot_key == key) {
            table->slots[idx].key = MEMO_TOMBSTONE;
            PyObject* value = table->slots[idx].value;
            table->slots[idx].value = NULL;
            table->used--;
            /* Return owned reference - caller owns it now */
            return value;
        }
        idx = (idx + 1) & mask;
    }
}

/* Pop an arbitrary item. Returns key via out parameter, value as return. */
static PyObject* memo_table_popitem(MemoTable* table, void** key_out) {
    if (!table || table->used == 0)
        return NULL;
    for (Py_ssize_t i = 0; i < table->size; i++) {
        void* slot_key = table->slots[i].key;
        if (slot_key && slot_key != MEMO_TOMBSTONE) {
            *key_out = slot_key;
            table->slots[i].key = MEMO_TOMBSTONE;
            PyObject* value = table->slots[i].value;
            table->slots[i].value = NULL;
            table->used--;
            return value; /* owned reference */
        }
    }
    return NULL;
}

/* ------------------------- Memo type & keepalive proxy -------------------------- */

PyTypeObject Memo_Type;

/* Forward declarations for view types */
static PyTypeObject MemoKeysView_Type;
static PyTypeObject MemoValuesView_Type;
static PyTypeObject MemoItemsView_Type;
static PyTypeObject MemoViewIter_Type;

/* _KeepaliveList proxy ============================================================
 * A thin Python object that forwards to PyMemoObject.keep.
 * It is *not* the owner of the storage; it keeps a strong ref to PyMemoObject.
 */

typedef struct {
    PyObject_HEAD PyMemoObject* owner; /* strong ref to the memo owning the vector */
} KeepaliveListObject;

static PyTypeObject KeepaliveList_Type;

/* Forward decls */
static PyObject* KeepaliveList_New(PyMemoObject* owner);

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

static PyObject* KeepaliveList_repr(KeepaliveListObject* self) {
    PyObject* list;
    PyObject* inner_repr;
    PyObject* wrapped;

    /* Equivalent to: list(self) */
    list = PySequence_List((PyObject*)self);
    if (list == NULL) {
        return NULL; /* Propagate any error (e.g. no owner, etc.) */
    }

    /* Equivalent to: repr(list(self)) */
    inner_repr = PyObject_Repr(list);
    Py_DECREF(list);
    if (inner_repr == NULL) {
        return NULL;
    }

    /* Wrap as keepalive([...]) */
    wrapped = PyUnicode_FromFormat("keepalive(%U)", inner_repr);
    Py_DECREF(inner_repr);
    return wrapped;
}

static PyMethodDef KeepaliveList_methods[] = {
    {"append", (PyCFunction)KeepaliveList_append, METH_O, NULL},
    {"clear", (PyCFunction)KeepaliveList_clear, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyObject* KeepaliveList_New(PyMemoObject* owner) {
    KeepaliveListObject* self = PyObject_New(KeepaliveListObject, &KeepaliveList_Type);
    if (!self)
        return NULL;
    Py_INCREF(owner);
    self->owner = owner;
    return (PyObject*)self;
}

static PyObject* KeepaliveList_richcompare(KeepaliveListObject* self, PyObject* other, int op) {
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    if (!PyObject_TypeCheck(other, &KeepaliveList_Type)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    KeepaliveListObject* other_list = (KeepaliveListObject*)other;
    int equal = (self->owner == other_list->owner);

    if (op == Py_EQ) {
        if (equal) {
            Py_RETURN_TRUE;
        }
        Py_RETURN_FALSE;
    } else { /* Py_NE */
        if (equal) {
            Py_RETURN_FALSE;
        }
        Py_RETURN_TRUE;
    }
}

static PyTypeObject KeepaliveList_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium.keepalive",
    .tp_basicsize = sizeof(KeepaliveListObject),
    .tp_dealloc = (destructor)KeepaliveList_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_as_sequence = &KeepaliveList_as_sequence,
    .tp_iter = (getiterfunc)KeepaliveList_iter,
    .tp_methods = KeepaliveList_methods,
    .tp_repr = (reprfunc)KeepaliveList_repr,
    .tp_richcompare = (richcmpfunc)KeepaliveList_richcompare,
    .tp_hash = PyObject_HashNotImplemented,
};

/* --------------------------- Memo View Types ------------------------------- */

typedef enum {
    MEMO_IT_KEYS = 0,
    MEMO_IT_VALUES = 1,
    MEMO_IT_ITEMS = 2
} MemoIterKind;

typedef struct {
    PyObject_HEAD PyMemoObject* owner; /* strong ref */
} MemoView;

typedef struct {
    PyObject_HEAD PyMemoObject* owner; /* strong ref */
    MemoIterKind kind;
    Py_ssize_t index;
    MemoTable* seen_table; /* for mutation detection */
    int keepalive_done;    /* 1 if keepalive was already yielded */
} MemoViewIter;

static void MemoView_dealloc(MemoView* self) {
    Py_XDECREF(self->owner);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int MemoView_traverse(MemoView* self, visitproc visit, void* arg) {
    Py_VISIT(self->owner);
    return 0;
}

static Py_ssize_t MemoView_len(MemoView* self) {
    Py_ssize_t count = 0;
    if (self->owner) {
        if (self->owner->table)
            count = self->owner->table->used;
        if (self->owner->keepalive.size > 0)
            count += 1;
    }
    return count;
}

static PyObject* MemoView_iter_for_kind(MemoView* view, MemoIterKind kind);

static PyObject* MemoKeysView_iter(PyObject* self) {
    return MemoView_iter_for_kind((MemoView*)self, MEMO_IT_KEYS);
}

static PyObject* MemoValuesView_iter(PyObject* self) {
    return MemoView_iter_for_kind((MemoView*)self, MEMO_IT_VALUES);
}

static PyObject* MemoItemsView_iter(PyObject* self) {
    return MemoView_iter_for_kind((MemoView*)self, MEMO_IT_ITEMS);
}

/* Repr for views */
static PyObject* MemoKeysView_repr(MemoView* self) {
    return PyUnicode_FromFormat("dict_keys(...)");
}

static PyObject* MemoValuesView_repr(MemoView* self) {
    return PyUnicode_FromFormat("dict_values(...)");
}

static PyObject* MemoItemsView_repr(MemoView* self) {
    return PyUnicode_FromFormat("dict_items(...)");
}

static PySequenceMethods MemoView_as_sequence = {
    .sq_length = (lenfunc)MemoView_len,
};

static PyTypeObject MemoKeysView_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "dict_keys",
    .tp_basicsize = sizeof(MemoView),
    .tp_dealloc = (destructor)MemoView_dealloc,
    .tp_repr = (reprfunc)MemoKeysView_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)MemoView_traverse,
    .tp_iter = (getiterfunc)MemoKeysView_iter,
    .tp_as_sequence = &MemoView_as_sequence,
};

static PyTypeObject MemoValuesView_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "dict_values",
    .tp_basicsize = sizeof(MemoView),
    .tp_dealloc = (destructor)MemoView_dealloc,
    .tp_repr = (reprfunc)MemoValuesView_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)MemoView_traverse,
    .tp_iter = (getiterfunc)MemoValuesView_iter,
    .tp_as_sequence = &MemoView_as_sequence,
};

static PyTypeObject MemoItemsView_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "dict_items",
    .tp_basicsize = sizeof(MemoView),
    .tp_dealloc = (destructor)MemoView_dealloc,
    .tp_repr = (reprfunc)MemoItemsView_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)MemoView_traverse,
    .tp_iter = (getiterfunc)MemoItemsView_iter,
    .tp_as_sequence = &MemoView_as_sequence,
};

/* View iterator */
static void MemoViewIter_dealloc(MemoViewIter* self) {
    Py_XDECREF(self->owner);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int MemoViewIter_traverse(MemoViewIter* self, visitproc visit, void* arg) {
    Py_VISIT(self->owner);
    return 0;
}

static PyObject* MemoViewIter_iternext(MemoViewIter* self) {
    if (!self->owner)
        return NULL;

    MemoTable* table = self->owner->table;

    /* Iterate over table entries if table exists */
    if (table) {
        /* Check for mutation */
        if (table != self->seen_table) {
            PyErr_SetString(PyExc_RuntimeError, "memo changed size during iteration");
            return NULL;
        }

        while (self->index < table->size) {
            Py_ssize_t idx = self->index++;
            void* slot_key = table->slots[idx].key;

            if (!slot_key || slot_key == MEMO_TOMBSTONE)
                continue;

            if (self->kind == MEMO_IT_KEYS) {
                return PyLong_FromVoidPtr(slot_key);
            } else if (self->kind == MEMO_IT_VALUES) {
                PyObject* value = table->slots[idx].value;
                return Py_NewRef(value);
            } else { /* MEMO_IT_ITEMS */
                PyObject* key_obj = PyLong_FromVoidPtr(slot_key);
                if (!key_obj)
                    return NULL;
                PyObject* value = table->slots[idx].value;
                PyObject* pair = PyTuple_New(2);
                if (!pair) {
                    Py_DECREF(key_obj);
                    return NULL;
                }
                PyTuple_SET_ITEM(pair, 0, key_obj);
                Py_INCREF(value);
                PyTuple_SET_ITEM(pair, 1, value);
                return pair;
            }
        }
    }

    /* Yield keepalive at end if non-empty and not yet done */
    if (!self->keepalive_done && self->owner->keepalive.size > 0) {
        self->keepalive_done = 1;
        void* keep_key = (void*)self->owner;

        if (self->kind == MEMO_IT_KEYS) {
            return PyLong_FromVoidPtr(keep_key);
        } else if (self->kind == MEMO_IT_VALUES) {
            return KeepaliveList_New(self->owner);
        } else { /* MEMO_IT_ITEMS */
            PyObject* key_obj = PyLong_FromVoidPtr(keep_key);
            if (!key_obj)
                return NULL;
            PyObject* value = KeepaliveList_New(self->owner);
            if (!value) {
                Py_DECREF(key_obj);
                return NULL;
            }
            PyObject* pair = PyTuple_New(2);
            if (!pair) {
                Py_DECREF(key_obj);
                Py_DECREF(value);
                return NULL;
            }
            PyTuple_SET_ITEM(pair, 0, key_obj);
            PyTuple_SET_ITEM(pair, 1, value);
            return pair;
        }
    }

    return NULL; /* StopIteration */
}

static PyTypeObject MemoViewIter_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium._memo_iterator",
    .tp_basicsize = sizeof(MemoViewIter),
    .tp_dealloc = (destructor)MemoViewIter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)MemoViewIter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)MemoViewIter_iternext,
};

static PyObject* MemoView_iter_for_kind(MemoView* view, MemoIterKind kind) {
    MemoViewIter* it = PyObject_GC_New(MemoViewIter, &MemoViewIter_Type);
    if (!it)
        return NULL;
    Py_INCREF(view->owner);
    it->owner = view->owner;
    it->kind = kind;
    it->index = 0;
    it->seen_table = view->owner ? view->owner->table : NULL;
    it->keepalive_done = 0;
    PyObject_GC_Track(it);
    return (PyObject*)it;
}

/* View constructors */
static PyObject* MemoKeysView_New(PyMemoObject* owner) {
    MemoView* v = PyObject_GC_New(MemoView, &MemoKeysView_Type);
    if (!v)
        return NULL;
    Py_INCREF(owner);
    v->owner = owner;
    PyObject_GC_Track(v);
    return (PyObject*)v;
}

static PyObject* MemoValuesView_New(PyMemoObject* owner) {
    MemoView* v = PyObject_GC_New(MemoView, &MemoValuesView_Type);
    if (!v)
        return NULL;
    Py_INCREF(owner);
    v->owner = owner;
    PyObject_GC_Track(v);
    return (PyObject*)v;
}

static PyObject* MemoItemsView_New(PyMemoObject* owner) {
    MemoView* v = PyObject_GC_New(MemoView, &MemoItemsView_Type);
    if (!v)
        return NULL;
    Py_INCREF(owner);
    v->owner = owner;
    PyObject_GC_Track(v);
    return (PyObject*)v;
}

/* --------------------------- Memo object impl ------------------------------ */

static void Memo_dealloc(PyMemoObject* self) {
    PyObject_GC_UnTrack(self);  // Stop tracking before destruction
    if (PyObject_CallFinalizerFromDealloc((PyObject*)self)) {
        return;
    }
    memo_table_free(self->table);
    keepalive_free(&self->keepalive);
    undo_log_free(&self->undo_log);
    PyObject_GC_Del(self);  // Use GC-aware free
}

static int Memo_traverse(PyMemoObject* self, visitproc visit, void* arg) {
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

static int Memo_clear_gc(PyMemoObject* self) {
    // Break all references
    if (self->table) {
        memo_table_clear(self->table);
    }
    keepalive_clear(&self->keepalive);
    return 0;
}

static ALWAYS_INLINE int memo_insert_logged(
    PyMemoObject* memo, void* key, PyObject* value, Py_ssize_t hash
);

PyMemoObject* Memo_New(void) {
    PyMemoObject* self = PyObject_GC_New(PyMemoObject, &Memo_Type);
    if (!self)
        return NULL;
    self->table = NULL;
    // Since memo is designed to be reused, unless stolen, don't call PyObject_GC_Track just yet.
    // Instead, call it once we know that somebody stole the ref.
    keepalive_init(&self->keepalive);
    undo_log_init(&self->undo_log);
    return self;
}

static Py_ssize_t Memo_len(PyMemoObject* self) {
    Py_ssize_t count = self->table ? self->table->used : 0;
    if (self->keepalive.size > 0)
        count += 1;
    return count;
}

static PyObject* Memo_subscript(PyMemoObject* self, PyObject* pykey) {
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

static int Memo_ass_subscript(PyMemoObject* self, PyObject* pykey, PyObject* value) {
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
        /* Use logged insert to support rollback for __deepcopy__ fallback */
        return memo_insert_logged(self, key, value, hash_pointer(key));
    }
}

static int Memo_contains(PyMemoObject* self, PyObject* pykey) {
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_TypeError, "keys must be integers");
        return -1;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return -1;

    /* Special-case for keepalive: memo[id(memo)] */
    if (key == (void*)self) {
        return self->keepalive.size > 0;
    }

    PyObject* value = memo_table_lookup(self->table, key);
    return value != NULL;
}

static PyObject* Memo_iter(PyMemoObject* self) {
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
    /* Append keepalive key at end if non-empty */
    if (self->keepalive.size > 0) {
        PyObject* keep_key = PyLong_FromVoidPtr((void*)self);
        if (!keep_key || PyList_Append(keys_list, keep_key) < 0) {
            Py_XDECREF(keep_key);
            Py_DECREF(keys_list);
            return NULL;
        }
        Py_DECREF(keep_key);
    }
    PyObject* it = PyObject_GetIter(keys_list);
    Py_DECREF(keys_list);
    return it;
}

static PyObject* Memo_clear(PyMemoObject* self, PyObject* noargs) {
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
static PyObject* Memo___del__(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    if (self->table) {
        memo_table_clear(self->table);
    }
    keepalive_clear(&self->keepalive);
    Py_RETURN_NONE;
}

static PyObject* Memo_contains_method(PyMemoObject* self, PyObject* pykey) {
    int result = Memo_contains(self, pykey);
    if (result < 0)
        return NULL;
    return PyBool_FromLong(result);
}

static PyObject* Memo_keep(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    /* Expose a (fresh) proxy each time; storage lives in self->keepalive. */
    return KeepaliveList_New(self);
}

/* keys() */
static PyObject* Memo_keys(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    return MemoKeysView_New(self);
}

/* values() */
static PyObject* Memo_values(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    return MemoValuesView_New(self);
}

/* items() */
static PyObject* Memo_items(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    return MemoItemsView_New(self);
}

/* copy() - shallow copy of the memo */
static PyObject* Memo_copy(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    PyMemoObject* new_memo = Memo_New();
    if (!new_memo)
        return NULL;

    if (self->table) {
        for (Py_ssize_t i = 0; i < self->table->size; i++) {
            void* key = self->table->slots[i].key;
            if (key && key != MEMO_TOMBSTONE) {
                PyObject* value = self->table->slots[i].value;
                if (memo_table_insert(&new_memo->table, key, value) < 0) {
                    Py_DECREF(new_memo);
                    return NULL;
                }
            }
        }
    }

    /* Copy keepalive too */
    for (Py_ssize_t i = 0; i < self->keepalive.size; i++) {
        if (keepalive_append(&new_memo->keepalive, self->keepalive.items[i]) < 0) {
            Py_DECREF(new_memo);
            return NULL;
        }
    }

    PyObject_GC_Track(new_memo);
    return (PyObject*)new_memo;
}

/* pop(key[, default]) */
static PyObject* Memo_pop(PyMemoObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "pop expected 1 or 2 arguments");
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

    PyObject* value = memo_table_pop(self->table, key);
    if (value) {
        return value; /* already owned */
    }

    if (nargs == 2) {
        return Py_NewRef(args[1]);
    }

    PyErr_SetObject(PyExc_KeyError, pykey);
    return NULL;
}

/* popitem() */
static PyObject* Memo_popitem(PyMemoObject* self, PyObject* noargs) {
    (void)noargs;
    void* key = NULL;
    PyObject* value = memo_table_popitem(self->table, &key);
    if (!value) {
        PyErr_SetString(PyExc_KeyError, "popitem(): memo is empty");
        return NULL;
    }

    PyObject* key_obj = PyLong_FromVoidPtr(key);
    if (!key_obj) {
        Py_DECREF(value);
        return NULL;
    }

    PyObject* pair = PyTuple_New(2);
    if (!pair) {
        Py_DECREF(key_obj);
        Py_DECREF(value);
        return NULL;
    }
    PyTuple_SET_ITEM(pair, 0, key_obj);
    PyTuple_SET_ITEM(pair, 1, value);
    return pair;
}

/* update([other], **kwds) - for memo, we only support mapping or iterable of pairs */
static PyObject* Memo_update(
    PyMemoObject* self, PyObject* const* args, Py_ssize_t nargs, PyObject* kwnames
) {
    /* Memo keys are integers (pointers), so kwargs don't make sense */
    if (kwnames && PyTuple_GET_SIZE(kwnames) > 0) {
        PyErr_SetString(PyExc_TypeError, "memo.update() does not accept keyword arguments");
        return NULL;
    }

    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "update expected at most 1 argument");
        return NULL;
    }

    if (nargs == 0) {
        Py_RETURN_NONE;
    }

    PyObject* other = args[0];

    /* Check if it's a Memo */
    if (Py_TYPE(other) == &Memo_Type) {
        PyMemoObject* other_memo = (PyMemoObject*)other;
        if (other_memo->table) {
            for (Py_ssize_t i = 0; i < other_memo->table->size; i++) {
                void* key = other_memo->table->slots[i].key;
                if (key && key != MEMO_TOMBSTONE) {
                    PyObject* value = other_memo->table->slots[i].value;
                    if (memo_table_insert(&self->table, key, value) < 0)
                        return NULL;
                }
            }
        }
        Py_RETURN_NONE;
    }

    /* Check if it has keys() - treat as mapping */
    PyObject* keys_method = PyObject_GetAttrString(other, "keys");
    if (keys_method) {
        Py_DECREF(keys_method);

        PyObject* keys = PyMapping_Keys(other);
        if (!keys)
            return NULL;

        PyObject* iter = PyObject_GetIter(keys);
        Py_DECREF(keys);
        if (!iter)
            return NULL;

        PyObject* key_obj;
        while ((key_obj = PyIter_Next(iter)) != NULL) {
            PyObject* value = PyObject_GetItem(other, key_obj);
            if (!value) {
                Py_DECREF(key_obj);
                Py_DECREF(iter);
                return NULL;
            }

            if (!PyLong_Check(key_obj)) {
                PyErr_SetString(PyExc_TypeError, "memo keys must be integers");
                Py_DECREF(value);
                Py_DECREF(key_obj);
                Py_DECREF(iter);
                return NULL;
            }

            void* key = PyLong_AsVoidPtr(key_obj);
            Py_DECREF(key_obj);
            if (key == NULL && PyErr_Occurred()) {
                Py_DECREF(value);
                Py_DECREF(iter);
                return NULL;
            }

            int rc = memo_table_insert(&self->table, key, value);
            Py_DECREF(value);
            if (rc < 0) {
                Py_DECREF(iter);
                return NULL;
            }
        }
        Py_DECREF(iter);
        if (PyErr_Occurred())
            return NULL;

        Py_RETURN_NONE;
    }
    PyErr_Clear();

    /* Treat as iterable of (key, value) pairs */
    PyObject* iter = PyObject_GetIter(other);
    if (!iter)
        return NULL;

    PyObject* item;
    while ((item = PyIter_Next(iter)) != NULL) {
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            PyErr_SetString(PyExc_TypeError, "update() items must be (key, value) pairs");
            Py_DECREF(item);
            Py_DECREF(iter);
            return NULL;
        }

        PyObject* key_obj = PyTuple_GET_ITEM(item, 0);
        PyObject* value = PyTuple_GET_ITEM(item, 1);

        if (!PyLong_Check(key_obj)) {
            PyErr_SetString(PyExc_TypeError, "memo keys must be integers");
            Py_DECREF(item);
            Py_DECREF(iter);
            return NULL;
        }

        void* key = PyLong_AsVoidPtr(key_obj);
        if (key == NULL && PyErr_Occurred()) {
            Py_DECREF(item);
            Py_DECREF(iter);
            return NULL;
        }

        int rc = memo_table_insert(&self->table, key, value);
        Py_DECREF(item);
        if (rc < 0) {
            Py_DECREF(iter);
            return NULL;
        }
    }
    Py_DECREF(iter);
    if (PyErr_Occurred())
        return NULL;

    Py_RETURN_NONE;
}

static PyMappingMethods Memo_as_mapping = {
    (lenfunc)Memo_len, (binaryfunc)Memo_subscript, (objobjargproc)Memo_ass_subscript
};

static PySequenceMethods Memo_as_sequence = {
    .sq_contains = (objobjproc)Memo_contains,
};

/* get(key[, default]) - returns None when key not found (dict-compatible) */
static PyObject* Memo_get(PyMemoObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "get expected 1 or 2 arguments");
        return NULL;
    }
    PyObject* pykey = args[0];
    if (!PyLong_Check(pykey)) {
        PyErr_SetString(PyExc_TypeError, "keys must be integers");
        return NULL;
    }
    void* key = PyLong_AsVoidPtr(pykey);
    if (key == NULL && PyErr_Occurred())
        return NULL;

    PyObject* value = memo_table_lookup(self->table, key);
    if (value) {
        return Py_NewRef(value);
    }

    /* Return default or None */
    if (nargs == 2) {
        return Py_NewRef(args[1]);
    }
    Py_RETURN_NONE;
}

static PyObject* Memo_setdefault(PyMemoObject* self, PyObject* const* args, Py_ssize_t nargs) {
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
        return Py_NewRef(value);
    }

    /* No existing value: store default (or None if omitted) and return it. */
    PyObject* def = (nargs == 2) ? args[1] : Py_None;
    if (memo_table_insert(&self->table, key, def) < 0) {
        return NULL;
    }
    return Py_NewRef(def);
}

/* __eq__ / __ne__ */
static PyObject* Memo_richcompare(PyMemoObject* self, PyObject* other, int op) {
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    /* Only compare with other Memo objects for now */
    if (Py_TYPE(other) != &Memo_Type) {
        /* Could also compare with dicts, but let's keep it simple */
        if (op == Py_EQ)
            Py_RETURN_FALSE;
        else
            Py_RETURN_TRUE;
    }

    PyMemoObject* other_memo = (PyMemoObject*)other;

    Py_ssize_t self_len = self->table ? self->table->used : 0;
    Py_ssize_t other_len = other_memo->table ? other_memo->table->used : 0;

    if (self_len != other_len) {
        if (op == Py_EQ)
            Py_RETURN_FALSE;
        else
            Py_RETURN_TRUE;
    }

    /* Check all keys and values match */
    if (self->table) {
        for (Py_ssize_t i = 0; i < self->table->size; i++) {
            void* key = self->table->slots[i].key;
            if (key && key != MEMO_TOMBSTONE) {
                PyObject* self_val = self->table->slots[i].value;
                PyObject* other_val = memo_table_lookup(other_memo->table, key);
                if (!other_val) {
                    if (op == Py_EQ)
                        Py_RETURN_FALSE;
                    else
                        Py_RETURN_TRUE;
                }
                int cmp = PyObject_RichCompareBool(self_val, other_val, Py_EQ);
                if (cmp < 0)
                    return NULL;
                if (!cmp) {
                    if (op == Py_EQ)
                        Py_RETURN_FALSE;
                    else
                        Py_RETURN_TRUE;
                }
            }
        }
    }

    if (op == Py_EQ)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject* Memo_as_dict(PyMemoObject* self) {
    /* self->table can be NULL for a freshly created memo that has
       not stored anything yet. In that case we just want an empty dict. */
    Py_ssize_t size = 0;
    if (self->table) {
        /* Use the number of live entries as a sizing hint. */
        size = self->table->used;
        if (size < 0) {
            size = 0;
        }
    }

    PyObject* dict = _PyDict_NewPresized(size);
    if (dict == NULL) {
        return NULL;
    }

    if (PyDict_Update(dict, (PyObject*)self) < 0) {
        Py_DECREF(dict);
        return NULL;
    }

    return dict;
}

/* __repr__ */
static PyObject* Memo_repr(PyMemoObject* self) {
    PyObject* dict = Memo_as_dict(self);
    PyObject* inner_repr;
    PyObject* wrapped;

    if (dict == NULL) {
        return NULL;
    }

    /* repr(dict) */
    inner_repr = PyObject_Repr(dict);
    Py_DECREF(dict);
    if (inner_repr == NULL) {
        return NULL;
    }

    /* Wrap as memo({...}) */
    wrapped = PyUnicode_FromFormat("memo(%U)", inner_repr);
    Py_DECREF(inner_repr);
    return wrapped;
}

static PyGetSetDef Memo_getset[] = {
    {"data", (getter)Memo_as_dict, NULL, "dict view of the memo contents", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyMethodDef Memo_methods[] = {
    {"clear", (PyCFunction)Memo_clear, METH_NOARGS, "Remove all items from the memo."},
    {"copy", (PyCFunction)Memo_copy, METH_NOARGS, "Return a shallow copy of the memo."},
    {"get",
     (PyCFunction)Memo_get,
     METH_FASTCALL,
     "Return the value for key if key is in the memo, else default."},
    {"items", (PyCFunction)Memo_items, METH_NOARGS, "Return a view of the memo's items."},
    {"keys", (PyCFunction)Memo_keys, METH_NOARGS, "Return a view of the memo's keys."},
    {"pop",
     (PyCFunction)Memo_pop,
     METH_FASTCALL,
     "Remove specified key and return the corresponding value."},
    {"popitem", (PyCFunction)Memo_popitem, METH_NOARGS, "Remove and return a (key, value) pair."},
    {"setdefault",
     (PyCFunction)Memo_setdefault,
     METH_FASTCALL,
     "Insert key with a value of default if key is not in the memo."},
    {"update",
     (PyCFunction)Memo_update,
     METH_FASTCALL | METH_KEYWORDS,
     "Update the memo from a mapping or iterable of pairs."},
    {"values", (PyCFunction)Memo_values, METH_NOARGS, "Return a view of the memo's values."},
    {"__contains__", (PyCFunction)Memo_contains_method, METH_O, "Return True if key is in memo."},
    {"keep", (PyCFunction)Memo_keep, METH_NOARGS, "Return the keepalive list proxy."},
    {"__del__", (PyCFunction)Memo___del__, METH_NOARGS, NULL},
    {NULL, NULL, 0, NULL}
};

PyTypeObject Memo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "copium.memo",
    .tp_basicsize = sizeof(PyMemoObject),
    .tp_dealloc = (destructor)Memo_dealloc,
    .tp_repr = (reprfunc)Memo_repr,
    .tp_getset = Memo_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_as_mapping = &Memo_as_mapping,
    .tp_as_sequence = &Memo_as_sequence,
    .tp_iter = (getiterfunc)Memo_iter,
    .tp_methods = Memo_methods,
    .tp_traverse = (traverseproc)Memo_traverse,
    .tp_clear = (inquiry)Memo_clear_gc,
    .tp_richcompare = (richcmpfunc)Memo_richcompare,
};

/* --------------------------- Type readiness helper ------------------------- */

/* Called from module init in _copying.c */
int memo_ready_types(void) {
    if (PyType_Ready(&KeepaliveList_Type) < 0)
        return -1;
    if (PyType_Ready(&MemoViewIter_Type) < 0)
        return -1;
    if (PyType_Ready(&MemoKeysView_Type) < 0)
        return -1;
    if (PyType_Ready(&MemoValuesView_Type) < 0)
        return -1;
    if (PyType_Ready(&MemoItemsView_Type) < 0)
        return -1;
    if (PyType_Ready(&Memo_Type) < 0)
        return -1;
    return 0;
}

/* Register Memo and view types with collections.abc ABCs */
int memo_register_abcs(void) {
    if (register_with_collections_abc("MutableMapping", &Memo_Type) < 0)
        return -1;
    register_with_collections_abc("KeysView", &MemoKeysView_Type);
    register_with_collections_abc("ValuesView", &MemoValuesView_Type);
    register_with_collections_abc("ItemsView", &MemoItemsView_Type);
    return 0;
}

static ALWAYS_INLINE PyMemoObject* get_tss_memo(void) {
    void* val = PyThread_tss_get(&module_state.memo_tss);
    if (val == NULL) {
        PyMemoObject* memo = Memo_New();
        if (memo == NULL)
            return NULL;
        if (PyThread_tss_set(&module_state.memo_tss, (void*)memo) != 0) {
            Py_DECREF(memo);
            Py_FatalError("copium: unexpected TTS state - failed to set memo");
        }
        return memo;
    }

    PyMemoObject* existing = (PyMemoObject*)val;
    if (Py_REFCNT(existing) > 1) {
        // Memo got stolen in between runs somehow.
        // Highly unlikely, but we'll detach it anyway and enable gc tracking for it.
        PyObject_GC_Track(existing);

        PyMemoObject* memo = Memo_New();
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

static ALWAYS_INLINE int cleanup_tss_memo(PyMemoObject* memo) {
    Py_ssize_t refcount = Py_REFCNT(memo);

    if (refcount == 1) {
        keepalive_clear(&memo->keepalive);
        keepalive_shrink_if_large(&memo->keepalive);
        undo_log_clear(&memo->undo_log);
        undo_log_shrink_if_large(&memo->undo_log);
        memo_table_reset(&memo->table);
        return 1;
    } else {
        PyObject_GC_Track(memo);
        if (PyThread_tss_set(&module_state.memo_tss, NULL) != 0) {
            Py_DECREF(memo);
            Py_FatalError("copium: unexpected TTS state during memo cleanup");
        }
        Py_DECREF(memo);
        return 0;
    }
}

/* Combined memo insert + keepalive. Returns 0 on success, -1 on error. */
static ALWAYS_INLINE int memoize(
    PyMemoObject* memo, PyObject* original, PyObject* copy, Py_ssize_t hash
) {
    if (memo_table_insert_h(&memo->table, (void*)original, copy, hash) < 0)
        return -1;
    if (keepalive_append(&memo->keepalive, original) < 0)
        return -1;
    return 0;
}

/* Remove object from memo on error cleanup. Keepalive entry remains (harmless).
   Returns 0 on success, -1 if not found. */
static int forget(PyMemoObject* memo, PyObject* original, Py_ssize_t hash) {
    return memo_table_remove_h(memo->table, (void*)original, hash);
}

/* --------------------- Checkpoint/Rollback Support ------------------------- */

typedef Py_ssize_t MemoCheckpoint;

static ALWAYS_INLINE MemoCheckpoint memo_checkpoint(PyMemoObject* memo) {
    return memo->undo_log.size;
}

static void memo_rollback(PyMemoObject* memo, MemoCheckpoint checkpoint) {
    Py_ssize_t end = memo->undo_log.size;
    for (Py_ssize_t i = checkpoint; i < end; i++) {
        void* key = memo->undo_log.keys[i];
        /* Remove from memo table. Ignore errors (entry might not exist
         * if it was already removed or if memoize partially failed). */
        memo_table_remove(memo->table, key);
    }
    memo->undo_log.size = checkpoint;
}

static ALWAYS_INLINE int memo_insert_logged(
    PyMemoObject* memo, void* key, PyObject* value, Py_ssize_t hash
) {
    int result = memo_table_insert_h(&memo->table, key, value, hash);
    if (result < 0)
        return -1;
    if (result == 0) {
        if (undo_log_append(&memo->undo_log, key) < 0)
            return -1;
    }
    return 0;
}

/* -------------------- Adaptive Fallback Helpers ---------------------------- */

static PyObject* memo_to_dict(PyMemoObject* memo) {
    Py_ssize_t size = memo->table ? memo->table->used : 0;
    PyObject* dict = _PyDict_NewPresized(size);
    if (!dict)
        return NULL;

    if (!memo->table)
        return dict;

    // Leave keepalive list out of it
    for (Py_ssize_t i = 0; i < memo->table->size; i++) {
        void* key = memo->table->slots[i].key;
        if (key && key != MEMO_TOMBSTONE) {
            PyObject* py_key = PyLong_FromVoidPtr(key);
            if (!py_key) {
                Py_DECREF(dict);
                return NULL;
            }
            PyObject* value = memo->table->slots[i].value;
            if (PyDict_SetItem(dict, py_key, value) < 0) {
                Py_DECREF(py_key);
                Py_DECREF(dict);
                return NULL;
            }
            Py_DECREF(py_key);
        }
    }

    return dict;
}

static int memo_sync_from_dict(PyMemoObject* memo, PyObject* dict, Py_ssize_t original_size) {
    Py_ssize_t current_size = PyDict_Size(dict);
    if (current_size < 0)
        return -1;

    if (current_size == original_size)
        return 0;

    PyObject *py_key, *value;
    Py_ssize_t pos = 0;
    Py_ssize_t idx = 0;

    while (PyDict_Next(dict, &pos, &py_key, &value)) {
        /* Skip first original_size entries—already in native memo */
        if (idx++ < original_size)
            continue;

        if (!PyLong_CheckExact(py_key))
            continue;

        void* key = PyLong_AsVoidPtr(py_key);
        if (key == NULL)
            return -1;

        /* Insert directly—no lookup needed, entry is new by construction */
        if (memo_table_insert(&memo->table, key, value) < 0)
            return -1;
    }

    return 0;
}
#endif  // _COPIUM_MEMO_C
