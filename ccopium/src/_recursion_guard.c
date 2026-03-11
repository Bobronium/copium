/*
 * SPDX-FileCopyrightText: 2025-present Arseny Boykov (Bobronium) <hi@bobronium.me>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _COPIUM_RECURSION_GUARD_C
#define _COPIUM_RECURSION_GUARD_C

#include <stddef.h> /* ptrdiff_t */
#if defined(__APPLE__) || defined(__linux__)
    #include <pthread.h>
#endif
#if defined(_WIN32)
    #include <windows.h>
#endif

#include "_common.h"

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
        GetStackLimitsFn fn = (GetStackLimitsFn)GetProcAddress(
            hKernel32, "GetCurrentThreadStackLimits"
        );
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

#endif  // _COPIUM_RECURSION_GUARD_C
