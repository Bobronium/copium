use pyo3_ffi::*;

use super::{ffi, PyTypeInfo};

pub unsafe trait PyVectorcallPtr {
    unsafe fn vectorcall_function(self) -> Option<vectorcallfunc>;
}

unsafe impl<T: PyTypeInfo> PyVectorcallPtr for *mut T {
    #[inline(always)]
    unsafe fn vectorcall_function(self) -> Option<vectorcallfunc> {
        ffi::PyVectorcall_Function(self as *mut PyObject)
    }
}

#[inline(always)]
pub fn nargs(nargsf: usize) -> Py_ssize_t {
    ffi::PyVectorcall_NARGS(nargsf)
}

#[inline(always)]
pub unsafe fn set_function_vectorcall<F: PyTypeInfo>(
    function: *mut F,
    vectorcall: vectorcallfunc,
) {
    #[cfg(Py_3_12)]
    unsafe {
        ffi::PyFunction_SetVectorcall(function as *mut PyObject, vectorcall);
    }

    #[cfg(not(Py_3_12))]
    {
        let _ = function;
        let _ = vectorcall;
    }
}
