use pyo3_ffi::*;

use super::{ffi, PyTypeInfo};

pub unsafe trait PyBoundMethodPtr {
    unsafe fn function(self) -> *mut PyObject;
    unsafe fn self_obj(self) -> *mut PyObject;
}

unsafe impl PyBoundMethodPtr for *mut ffi::PyMethodObject {
    #[inline(always)]
    unsafe fn function(self) -> *mut PyObject {
        ffi::PyMethod_GET_FUNCTION(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn self_obj(self) -> *mut PyObject {
        ffi::PyMethod_GET_SELF(self as *mut PyObject)
    }
}

#[inline(always)]
pub unsafe fn new<F: PyTypeInfo, S: PyTypeInfo>(
    function: *mut F,
    self_object: *mut S,
) -> *mut ffi::PyMethodObject {
    ffi::PyMethod_New(function as *mut PyObject, self_object as *mut PyObject)
        as *mut ffi::PyMethodObject
}
