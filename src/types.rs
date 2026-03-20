pub use crate::py::ffi::PyMethodObject;
pub use crate::py::object::{PyObjectPtr, PyObjectSlotPtr};
pub use crate::py::type_object::PyTypeObjectPtr;
pub use crate::py::{
    PyFrozensetObject, PyTypeInfo,
    bytearray::PyBufPtr,
    dict::PyMapPtr,
    list::PyMutSeqPtr,
    method::PyBoundMethodPtr,
    seq::PySeqPtr,
    set::{PyMutSetPtr, PySetPtr},
};

use pyo3_ffi::*;

#[inline(always)]
pub unsafe fn py_list_new(length: Py_ssize_t) -> *mut PyListObject {
    crate::py::list::new(length)
}

#[inline(always)]
pub unsafe fn py_tuple_new(length: Py_ssize_t) -> *mut PyTupleObject {
    crate::py::tuple::new(length)
}

#[inline(always)]
pub unsafe fn py_dict_new(length: Py_ssize_t) -> *mut PyDictObject {
    crate::py::dict::new_presized(length)
}

#[inline(always)]
pub unsafe fn py_set_new() -> *mut PySetObject {
    crate::py::set::new()
}

#[inline(always)]
pub unsafe fn py_set_from<T: PyTypeInfo>(iterable: *mut T) -> *mut PySetObject {
    crate::py::set::from(iterable)
}

#[inline(always)]
pub unsafe fn frozenset_from<T: PyTypeInfo>(iterable: *mut T) -> *mut PyFrozensetObject {
    crate::py::set::frozen_from(iterable)
}

#[inline(always)]
pub unsafe fn py_bytearray_new(length: Py_ssize_t) -> *mut PyByteArrayObject {
    crate::py::bytearray::new(length)
}

#[inline(always)]
pub unsafe fn py_method_new<F: PyTypeInfo, S: PyTypeInfo>(
    function: *mut F,
    self_object: *mut S,
) -> *mut PyMethodObject {
    crate::py::method::new(function, self_object)
}
