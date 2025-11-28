#ifndef _COPIUM_TYPE_CHECKS_C
#define _COPIUM_TYPE_CHECKS_C

#include "_state.c"
#include "pycore_object.h" /* _PyNone_Type, _PyNotImplemented_Type */

static ALWAYS_INLINE int is_literal_immutable(PyTypeObject* tp) {
    unsigned long r = (tp == &_PyNone_Type) | (tp == &PyLong_Type) | (tp == &PyUnicode_Type) |
        (tp == &PyBool_Type) | (tp == &PyFloat_Type) | (tp == &PyBytes_Type);
    return (int)r;
}

static ALWAYS_INLINE int is_builtin_immutable(PyTypeObject* tp) {
    unsigned long r = (tp == &PyRange_Type) | (tp == &PyFunction_Type) | (tp == &PyCFunction_Type) |
        (tp == &PyProperty_Type) | (tp == &_PyWeakref_RefType) | (tp == &PyCode_Type) |
        (tp == &PyModule_Type) | (tp == &_PyNotImplemented_Type) | (tp == &PyEllipsis_Type) |
        (tp == &PyComplex_Type);
    return (int)r;
}

static ALWAYS_INLINE int is_stdlib_immutable(PyTypeObject* tp) {
    unsigned long r = (tp == module_state.re_Pattern_type) | (tp == module_state.Decimal_type) |
        (tp == module_state.Fraction_type);
    return (int)r;
}

static ALWAYS_INLINE int is_class(PyTypeObject* tp) {
    return PyType_HasFeature(tp, Py_TPFLAGS_TYPE_SUBCLASS);
}

static ALWAYS_INLINE int is_atomic_immutable(PyTypeObject* tp) {
    unsigned long r = (unsigned long)is_literal_immutable(tp) |
        (unsigned long)is_builtin_immutable(tp) | (unsigned long)is_class(tp) |
        (unsigned long)is_stdlib_immutable(tp);
    return (int)r;
}

#endif  // _COPIUM_TYPE_CHECKS_C
