/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * Public header for copium memo/keepalive internals used by _copying.c
 */
#ifndef COPIUM_MEMO_H
#define COPIUM_MEMO_H

#include "Python.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------ Public data types --------------------------- */

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
} KeepVector;

/* Exact runtime layout of the memo object (must begin with PyObject_HEAD). */
typedef struct _MemoObject {
    PyObject_HEAD MemoTable* table;
    KeepVector keep;
} MemoObject;

/* ------------------------------ Public symbols ----------------------------- */

/* Memo type object and constructor */
extern PyTypeObject Memo_Type;
PyObject* Memo_New(void);

/* Pointer hasher shared by _copying.c to avoid recomputation */
Py_ssize_t memo_hash_pointer(void* ptr);

/* Memo table management */
void memo_table_free(MemoTable* table);
void memo_table_clear(MemoTable* table);
int memo_table_reset(MemoTable** table_ptr);

/* Lookup/insert (generic) */
PyObject* memo_table_lookup(MemoTable* table, void* key);
int memo_table_insert(MemoTable** table_ptr, void* key, PyObject* value);
int memo_table_remove(MemoTable* table, void* key);

/* Lookup/insert with precomputed hash (hot path) */
PyObject* memo_table_lookup_h(MemoTable* table, void* key, Py_ssize_t hash);
int memo_table_insert_h(MemoTable** table_ptr, void* key, PyObject* value, Py_ssize_t hash);

/* Keepalive vector helpers */
void keepvector_clear(KeepVector* kv);
int keepvector_append(KeepVector* kv, PyObject* obj);
void keepvector_shrink_if_large(KeepVector* kv);

/* Initialize Memo/_KeepList types */
int memo_ready_types(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COPIUM_MEMO_H */