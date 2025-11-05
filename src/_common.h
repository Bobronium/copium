/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * Common utilities, macros, and definitions shared across copium C extensions
 */
#ifndef COPIUM_COMMON_H
#define COPIUM_COMMON_H

#include <stdint.h>
#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== Compiler hints and attributes ===================== */

#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
    #define ALWAYS_INLINE __forceinline
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
    #define ALWAYS_INLINE inline
#endif

/* ======================== Common constants =================================== */

/* Tombstone marker for hash tables (pointer that will never be valid) */
#define HASH_TABLE_TOMBSTONE ((void*)(uintptr_t)(-1))

/* Hash table load factor thresholds */
#define HASH_TABLE_LOAD_FACTOR_NUMERATOR   10
#define HASH_TABLE_LOAD_FACTOR_DENOMINATOR 7

/* Initial hash table size */
#define HASH_TABLE_INITIAL_SIZE 8

/* ======================== Hash function ====================================== */

/**
 * SplitMix64-style pointer hasher.
 * Provides stable hashing across the process lifetime.
 *
 * @param ptr Pointer to hash
 * @return Hash value suitable for use as hash table index
 */
static ALWAYS_INLINE Py_ssize_t hash_pointer(void* ptr) {
    uintptr_t h = (uintptr_t)ptr;
    h ^= h >> 33;
    h *= (uintptr_t)0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (Py_ssize_t)h;
}

/* ======================== Hash table operation macros ======================= */

/**
 * Check if hash table needs resizing based on load factor.
 * Usage: if (HASH_TABLE_SHOULD_RESIZE(table)) { resize(); }
 */
#define HASH_TABLE_SHOULD_RESIZE(table) \
    ((table)->filled * HASH_TABLE_LOAD_FACTOR_NUMERATOR >= \
     (table)->size * HASH_TABLE_LOAD_FACTOR_DENOMINATOR)

/**
 * Calculate next power-of-two size for hash table growth.
 * Includes overflow protection.
 */
#define HASH_TABLE_NEXT_SIZE(current, min_needed) ({ \
    Py_ssize_t _new_size = (current) > 0 ? (current) : HASH_TABLE_INITIAL_SIZE; \
    while (_new_size < (min_needed) * 2) { \
        Py_ssize_t _next = _new_size << 1; \
        if (_next <= 0 || _next < _new_size) { \
            _new_size = (Py_ssize_t)1 << (sizeof(void*) * 8 - 2); \
            break; \
        } \
        _new_size = _next; \
    } \
    _new_size; \
})

/**
 * Linear probing step for hash table lookups.
 * Usage: idx = HASH_TABLE_PROBE_NEXT(idx, mask);
 */
#define HASH_TABLE_PROBE_NEXT(idx, mask) (((idx) + 1) & (mask))

/**
 * Check if a hash table slot key is empty.
 */
#define HASH_TABLE_IS_EMPTY(key) (!(key))

/**
 * Check if a hash table slot is a tombstone.
 */
#define HASH_TABLE_IS_TOMBSTONE(key) ((key) == HASH_TABLE_TOMBSTONE)

/**
 * Check if a hash table slot is valid (not empty and not tombstone).
 */
#define HASH_TABLE_IS_VALID(key) ((key) && (key) != HASH_TABLE_TOMBSTONE)

/* ======================== Python API utility macros ========================= */

/**
 * Safe DECREF that checks for NULL before decrementing.
 * Replaces common Py_XDECREF patterns.
 */
#define SAFE_DECREF(obj) do { \
    if ((obj)) { \
        Py_DECREF(obj); \
        (obj) = NULL; \
    } \
} while (0)

/**
 * Safe INCREF that checks for NULL.
 * Returns 0 on success, -1 if obj is NULL.
 */
#define SAFE_INCREF(obj) ({ \
    int _result = 0; \
    if (!(obj)) { \
        _result = -1; \
    } else { \
        Py_INCREF(obj); \
    } \
    _result; \
})

/**
 * Check if a Python object is of expected type.
 * Sets TypeError and returns -1 on mismatch.
 */
#define CHECK_TYPE(obj, type_check, type_name) do { \
    if (!(type_check)(obj)) { \
        PyErr_Format(PyExc_TypeError, \
                     "expected %s, got %.100s", \
                     type_name, \
                     Py_TYPE(obj)->tp_name); \
        return NULL; \
    } \
} while (0)

/**
 * Validate argument count for FASTCALL functions.
 * Sets TypeError and returns NULL on mismatch.
 */
#define CHECK_ARG_COUNT(nargs, expected, func_name) do { \
    if ((nargs) != (expected)) { \
        PyErr_Format(PyExc_TypeError, \
                     "%s expected %d argument%s, got %zd", \
                     func_name, \
                     expected, \
                     (expected) == 1 ? "" : "s", \
                     nargs); \
        return NULL; \
    } \
} while (0)

/**
 * Validate argument count range for FASTCALL functions.
 * Sets TypeError and returns NULL if out of range.
 */
#define CHECK_ARG_COUNT_RANGE(nargs, min, max, func_name) do { \
    if ((nargs) < (min) || (nargs) > (max)) { \
        PyErr_Format(PyExc_TypeError, \
                     "%s expected %d to %d arguments, got %zd", \
                     func_name, min, max, nargs); \
        return NULL; \
    } \
} while (0)

/* ======================== Memory allocation helpers ========================= */

/**
 * Allocate and zero-initialize memory.
 * Returns NULL on allocation failure.
 */
#define ALLOC_ZEROED(type, count) \
    ((type*)calloc((size_t)(count), sizeof(type)))

/**
 * Reallocate memory safely, preserving old pointer on failure.
 */
#define SAFE_REALLOC(ptr, new_size, cleanup_on_fail) ({ \
    void* _new_ptr = PyMem_Realloc((ptr), (size_t)(new_size)); \
    if (!_new_ptr && (new_size) > 0) { \
        cleanup_on_fail; \
    } else { \
        (ptr) = _new_ptr; \
    } \
    _new_ptr; \
})

/* ======================== Iteration helpers ================================== */

/**
 * Standard for-loop over hash table slots.
 * Usage: FOR_EACH_HASH_SLOT(i, table->size) { ... }
 */
#define FOR_EACH_HASH_SLOT(var, size) \
    for (Py_ssize_t var = 0; var < (size); var++)

/**
 * Iterate over valid (non-empty, non-tombstone) hash table entries.
 * Usage: FOR_EACH_VALID_ENTRY(i, table->slots, table->size, entry_type) { ... }
 */
#define FOR_EACH_VALID_ENTRY(var, slots, size) \
    FOR_EACH_HASH_SLOT(var, size) \
        if (HASH_TABLE_IS_VALID((slots)[var].key))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COPIUM_COMMON_H */
