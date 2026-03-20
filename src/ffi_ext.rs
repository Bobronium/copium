#![allow(non_snake_case)]
#![allow(unused_imports)]

pub use crate::py::ffi::{
    _PyNone_Type, _PyNotImplemented_Type, _PySet_NextEntry, _PyWeakref_RefType, PyEllipsis_Type,
    PyErr_Format, PyMethod_GET_FUNCTION, PyMethod_GET_SELF, PyMethod_New, PyMethod_Type,
    PyMethodObject, PyObject_CallFunction, PyObject_CallFunctionObjArgs,
    PyObject_CallMethodObjArgs, PySequence_Fast, PySequence_Fast_GET_ITEM,
    PySequence_Fast_GET_SIZE, PyUnicode_FromFormat, PyVectorcall_Function, PyVectorcall_NARGS,
    Py_None, tp_flags_of,
};

#[cfg(Py_3_12)]
pub use crate::py::ffi::PyFunction_SetVectorcall;
