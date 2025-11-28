#ifndef _COPIUM_REDUCE_HELPERS_C
#define _COPIUM_REDUCE_HELPERS_C

#include "_state.c"


typedef enum {
    REDUCE_ERROR = -1,
    REDUCE_TUPLE = 0,
    REDUCE_STRING = 1
} ReduceValidation;

// Try to find a reducer in copyreg.dispatch_table for the given type.
//
// Returns:
// - New reference to reduce result on success (reducer found and called)
// - NULL with PyErr_Occurred() on error (reducer failed)
// - NULL without error if type not in dispatch table (caller should try __reduce_ex__)
//
static PyObject* try_reduce_via_registry(PyObject* obj, PyTypeObject* tp) {
    PyObject* reducer = PyDict_GetItemWithError(module_state.copyreg_dispatch, (PyObject*)tp);
    if (!reducer) {
        // NULL can mean "not found" or "error during lookup"
        if (PyErr_Occurred())
            return NULL;
        return NULL;  // not found, no error - caller should try __reduce_ex__
    }
    if (!PyCallable_Check(reducer)) {
        PyErr_SetString(PyExc_TypeError, "copyreg.dispatch_table value is not callable");
        return NULL;
    }
    return PyObject_CallOneArg(reducer, obj);
}

static PyObject* call_reduce_method_preferring_ex(PyObject* obj) {
    PyObject* reduce_ex = NULL;
    int has_reduce_ex = PyObject_GetOptionalAttr(obj, module_state.str_reduce_ex, &reduce_ex);
    if (has_reduce_ex > 0) {
        PyObject* res = PyObject_CallFunction(reduce_ex, "i", 4);
        Py_DECREF(reduce_ex);
        return res;
    }
    if (has_reduce_ex < 0)
        return NULL;

    PyObject* reduce = NULL;
    int has_reduce = PyObject_GetOptionalAttr(obj, module_state.str_reduce, &reduce);
    if (has_reduce > 0) {
        PyObject* res = PyObject_CallNoArgs(reduce);
        Py_DECREF(reduce);
        return res;
    }
    if (has_reduce < 0)
        return NULL;

    PyErr_SetString(
        (PyObject*)module_state.copy_Error, "un(deep)copyable object (no reduce protocol)"
    );
    return NULL;
}

static int validate_reduce_tuple(
    PyObject* reduce_result,
    PyObject** out_callable,
    PyObject** out_argtup,
    PyObject** out_state,
    PyObject** out_listitems,
    PyObject** out_dictitems
) {
    if (!PyTuple_Check(reduce_result)) {
        if (PyUnicode_Check(reduce_result) || PyBytes_Check(reduce_result)) {
            *out_callable = *out_argtup = *out_state = *out_listitems = *out_dictitems = NULL;
            return REDUCE_STRING;
        }
        PyErr_SetString(PyExc_TypeError, "__reduce__ must return a tuple or str");
        return REDUCE_ERROR;
    }

    Py_ssize_t size = PyTuple_GET_SIZE(reduce_result);
    if (size < 2 || size > 5) {
        PyErr_SetString(
            PyExc_TypeError, "tuple returned by __reduce__ must contain 2 through 5 elements"
        );
        return REDUCE_ERROR;
    }

    PyObject* callable = PyTuple_GET_ITEM(reduce_result, 0);
    PyObject* argtup = PyTuple_GET_ITEM(reduce_result, 1);
    PyObject* state = (size >= 3) ? PyTuple_GET_ITEM(reduce_result, 2) : Py_None;
    PyObject* listitems = (size >= 4) ? PyTuple_GET_ITEM(reduce_result, 3) : Py_None;
    PyObject* dictitems = (size == 5) ? PyTuple_GET_ITEM(reduce_result, 4) : Py_None;

    if (!PyCallable_Check(callable)) {
        PyErr_Format(
            PyExc_TypeError,
            "first item of the tuple returned by __reduce__ must be callable, not %.200s",
            Py_TYPE(callable)->tp_name
        );
        return REDUCE_ERROR;
    }

    if (!PyTuple_Check(argtup)) {
        PyErr_Format(
            PyExc_TypeError,
            "second item of the tuple returned by __reduce__ must be a tuple, not %.200s",
            Py_TYPE(argtup)->tp_name
        );
        return REDUCE_ERROR;
    }

    if (listitems != Py_None && !PyIter_Check(listitems)) {
        PyErr_Format(
            PyExc_TypeError,
            "fourth item of the tuple returned by __reduce__ must be an iterator, not %.200s",
            Py_TYPE(listitems)->tp_name
        );
        return REDUCE_ERROR;
    }

    if (dictitems != Py_None && !PyIter_Check(dictitems)) {
        PyErr_Format(
            PyExc_TypeError,
            "fifth item of the tuple returned by __reduce__ must be an iterator, not %.200s",
            Py_TYPE(dictitems)->tp_name
        );
        return REDUCE_ERROR;
    }

    *out_callable = callable;
    *out_argtup = argtup;
    *out_state = (state == Py_None) ? NULL : state;
    *out_listitems = (listitems == Py_None) ? NULL : listitems;
    *out_dictitems = (dictitems == Py_None) ? NULL : dictitems;

    return REDUCE_TUPLE;
}

#endif  // _COPIUM_REDUCE_HELPERS_C