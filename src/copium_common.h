/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef Py_BUILD_CORE_MODULE
#  define Py_BUILD_CORE_MODULE
#endif
#ifndef PY_SSIZE_T_CLEAN
    #define PY_SSIZE_T_CLEAN
#endif

#ifndef COPIUM_COMMON_H
#define COPIUM_COMMON_H

#define PY_VERSION_3_11_HEX 0x030B0000
#define PY_VERSION_3_12_HEX 0x030C0000
#define PY_VERSION_3_13_HEX 0x030D0000
#define PY_VERSION_3_14_HEX 0x030E0000


#ifndef LIKELY
    #if defined(__GNUC__) || defined(__clang__)
        #define LIKELY(x) __builtin_expect(!!(x), 1)
        #define UNLIKELY(x) __builtin_expect(!!(x), 0)
    #else
        #define LIKELY(x) (x)
        #define UNLIKELY(x) (x)
    #endif
#endif

#ifndef UNLIKELY
    #define UNLIKELY(x) (x)
#endif

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
    #if defined(__GNUC__) || defined(__clang__)
        #define COPIUM_ATTR_HOT __attribute__((hot))
        #define COPIUM_ATTR_GNU_INLINE __attribute__((gnu_inline))
    #else
        #define COPIUM_ATTR_HOT
        #define COPIUM_ATTR_GNU_INLINE
    #endif
#endif

#if defined(_MSC_VER)
    #define ALWAYS_INLINE __forceinline
    #define MAYBE_INLINE __inline
#elif defined(__GNUC__) || defined(__clang__)
    #define ALWAYS_INLINE \
        inline __attribute__((always_inline)) COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
    #define MAYBE_INLINE inline COPIUM_ATTR_HOT COPIUM_ATTR_GNU_INLINE
#else
    #define ALWAYS_INLINE inline
    #define MAYBE_INLINE inline
#endif

#include <Python.h>

#endif /* COPIUM_COMMON_H */