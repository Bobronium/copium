use pyo3_ffi::*;
use std::ffi::{c_void, CStr};

use super::PyTypeInfo;

pub unsafe trait PyCapsulePtr {
    unsafe fn get_pointer(self, name: &CStr) -> *mut c_void;
}

unsafe impl<T: PyTypeInfo> PyCapsulePtr for *mut T {
    #[inline(always)]
    unsafe fn get_pointer(self, name: &CStr) -> *mut c_void {
        pyo3_ffi::PyCapsule_GetPointer(self as *mut PyObject, name.as_ptr())
    }
}

#[inline(always)]
pub unsafe fn new(
    pointer: *mut c_void,
    name: &CStr,
    destructor: Option<PyCapsule_Destructor>,
) -> *mut PyObject {
    pyo3_ffi::PyCapsule_New(pointer, name.as_ptr(), destructor)
}
