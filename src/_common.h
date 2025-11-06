/*
 * SPDX-FileCopyrightText: 2023-present Arseny Boykov
 * SPDX-License-Identifier: MIT
 *
 * Common utilities, macros, and definitions shared across copium C extensions.
 * This header provides a coherent foundation for all copium C modules.
 */
#ifndef COPIUM_COMMON_H
#define COPIUM_COMMON_H

#include <stdint.h>
#include "Python.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * SECTION 1: Compiler Hints and Attributes
 * ============================================================================ */

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

/* MAYBE_INLINE: Strong hint for PGO-driven inlining, but not forced */
#if defined(_MSC_VER)
    #define MAYBE_INLINE __inline
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(__has_attribute)
        #if __has_attribute(hot)
            #define COPIUM_ATTR_HOT __attribute__((hot))
        #else
            #define COPIUM_ATTR_HOT
        #endif
        #if __has_attribute(gnu_inline)
            #define COPIUM_ATTR_GNU_INLINE __attribute__((gnu_inline))
        #else
            #define COPIUM_ATTR_GNU_INLINE
        #endif
    #else
        #define COPIUM_ATTR_HOT __attribute__((hot))
        #define COPIUM_ATTR_GNU_INLINE __attribute__((gnu_inline))
    #endif
    #define MAYBE_INLINE inline COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
#else
    #define MAYBE_INLINE inline
#endif

/* ============================================================================
 * SECTION 2: Hash Table Utilities
 * ============================================================================ */

/* Tombstone marker for deleted hash table entries */
#define HASH_TABLE_TOMBSTONE ((void*)(uintptr_t)(-1))

/* Load factor: resize when (filled * 10) >= (size * 7) */
#define HASH_TABLE_LOAD_FACTOR_NUM   10
#define HASH_TABLE_LOAD_FACTOR_DENOM 7
#define HASH_TABLE_INITIAL_SIZE      8

/**
 * SplitMix64-style pointer hasher.
 * Provides stable, high-quality hashing across the process lifetime.
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

/* Check if hash table needs resizing */
#define HASH_TABLE_SHOULD_RESIZE(table) \
    ((table)->filled * HASH_TABLE_LOAD_FACTOR_NUM >= \
     (table)->size * HASH_TABLE_LOAD_FACTOR_DENOM)

/* Linear probing: advance to next slot */
#define HASH_TABLE_PROBE_NEXT(idx, mask) (((idx) + 1) & (mask))

/* Slot state checks */
#define HASH_TABLE_IS_EMPTY(key)      (!(key))
#define HASH_TABLE_IS_TOMBSTONE(key)  ((key) == HASH_TABLE_TOMBSTONE)
#define HASH_TABLE_IS_VALID(key)      ((key) && (key) != HASH_TABLE_TOMBSTONE)

/* Calculate next power-of-two size with overflow protection */
#define HASH_TABLE_NEXT_SIZE(current, min_needed) ({ \
    Py_ssize_t _size = (current) > 0 ? (current) : HASH_TABLE_INITIAL_SIZE; \
    while (_size < (min_needed) * 2) { \
        Py_ssize_t _next = _size << 1; \
        if (_next <= 0 || _next < _size) { \
            _size = (Py_ssize_t)1 << (sizeof(void*) * 8 - 2); \
            break; \
        } \
        _size = _next; \
    } \
    _size; \
})

/* Iteration over hash table slots */
#define FOR_EACH_HASH_SLOT(var, size) \
    for (Py_ssize_t var = 0; var < (size); var++)

#define FOR_EACH_VALID_ENTRY(var, slots, size) \
    FOR_EACH_HASH_SLOT(var, size) \
        if (HASH_TABLE_IS_VALID((slots)[var].key))

/* ============================================================================
 * SECTION 3: Memory Allocation Helpers
 * ============================================================================ */

/* Allocate and zero-initialize memory */
#define ALLOC_ZEROED(type, count) \
    ((type*)calloc((size_t)(count), sizeof(type)))

/* Safe reallocation with old pointer preserved on failure */
#define SAFE_REALLOC(ptr, new_size, cleanup_on_fail) ({ \
    void* _new = PyMem_Realloc((ptr), (size_t)(new_size)); \
    if (!_new && (new_size) > 0) { \
        cleanup_on_fail; \
    } else { \
        (ptr) = _new; \
    } \
    _new; \
})

/* ============================================================================
 * SECTION 4: Python Object Utilities
 * ============================================================================ */

/* Safe reference counting */
#define SAFE_DECREF(obj) do { \
    if ((obj)) { \
        Py_DECREF(obj); \
        (obj) = NULL; \
    } \
} while (0)

#define SAFE_INCREF(obj) ({ \
    int _result = 0; \
    if (!(obj)) { \
        _result = -1; \
    } else { \
        Py_INCREF(obj); \
    } \
    _result; \
})

/* Type checking with automatic error messages */
#define CHECK_TYPE_OR_RETURN_NULL(obj, check, typename) do { \
    if (!(check)(obj)) { \
        PyErr_Format(PyExc_TypeError, \
                     "expected %s, got %.100s", \
                     typename, \
                     Py_TYPE(obj)->tp_name); \
        return NULL; \
    } \
} while (0)

/* Argument count validation for FASTCALL functions */
#define CHECK_NARGS(nargs, expected, funcname) do { \
    if ((nargs) != (expected)) { \
        PyErr_Format(PyExc_TypeError, \
                     "%s() takes %d argument%s (%zd given)", \
                     funcname, expected, \
                     (expected) == 1 ? "" : "s", \
                     nargs); \
        return NULL; \
    } \
} while (0)

#define CHECK_NARGS_RANGE(nargs, min, max, funcname) do { \
    if ((nargs) < (min) || (nargs) > (max)) { \
        PyErr_Format(PyExc_TypeError, \
                     "%s() takes %d to %d arguments (%zd given)", \
                     funcname, min, max, nargs); \
        return NULL; \
    } \
} while (0)

/* Convert pointer to Python int (for memo keys) */
#define PTR_TO_PYLONG(ptr) PyLong_FromVoidPtr(ptr)
#define PYLONG_TO_PTR(obj) PyLong_AsVoidPtr(obj)

/* ============================================================================
 * SECTION 5: Common Error Handling Patterns
 * ============================================================================ */

/* Return NULL with memory error */
#define RETURN_NO_MEMORY() do { \
    PyErr_NoMemory(); \
    return NULL; \
} while (0)

/* Return NULL with runtime error */
#define RETURN_RUNTIME_ERROR(msg) do { \
    PyErr_SetString(PyExc_RuntimeError, msg); \
    return NULL; \
} while (0)

/* Return NULL with type error */
#define RETURN_TYPE_ERROR(msg) do { \
    PyErr_SetString(PyExc_TypeError, msg); \
    return NULL; \
} while (0)

/* Clean up and return NULL */
#define CLEANUP_AND_RETURN_NULL(...) do { \
    __VA_ARGS__; \
    return NULL; \
} while (0)

/* ============================================================================
 * SECTION 6: Memo/Keepalive Common Patterns
 * ============================================================================ */

/**
 * Common pattern for memo lookup with memoization.
 * Usage: MEMO_CHECK_AND_RETURN(obj, lookup_expr)
 */
#define MEMO_CHECK_AND_RETURN(obj, lookup_expr) do { \
    PyObject* _cached = (lookup_expr); \
    if (_cached) { \
        Py_INCREF(_cached); \
        return _cached; \
    } \
    if (PyErr_Occurred()) \
        return NULL; \
} while (0)

/**
 * Common pattern for keepalive append after copy.
 * Usage: KEEPALIVE_APPEND_IF_DIFFERENT(copy, src, append_expr)
 */
#define KEEPALIVE_APPEND_IF_DIFFERENT(copy, src, append_expr) do { \
    if ((copy) != (src)) { \
        if ((append_expr) < 0) { \
            Py_DECREF(copy); \
            return NULL; \
        } \
    } \
} while (0)

/* ============================================================================
 * SECTION 7: Loop Constructs
 * ============================================================================ */

/* Iterate over Python list items */
#define FOR_EACH_LIST_ITEM(i, list, n) \
    for (Py_ssize_t i = 0; i < (n); i++)

/* Iterate over Python tuple items */
#define FOR_EACH_TUPLE_ITEM(i, tuple, n) \
    for (Py_ssize_t i = 0; i < (n); i++)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COPIUM_COMMON_H */
