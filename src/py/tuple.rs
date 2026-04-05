use pyo3_ffi::*;

#[inline(always)]
pub unsafe fn new(length: Py_ssize_t) -> *mut PyTupleObject {
    pyo3_ffi::PyTuple_New(length) as *mut PyTupleObject
}
