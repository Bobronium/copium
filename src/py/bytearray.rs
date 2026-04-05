use pyo3_ffi::*;

pub unsafe trait PyBufPtr: Sized {
    unsafe fn length(self) -> Py_ssize_t;
    unsafe fn as_mut_ptr(self) -> *mut u8;

    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        self.length()
    }

    #[inline(always)]
    unsafe fn as_ptr(self) -> *mut u8 {
        self.as_mut_ptr()
    }
}

unsafe impl PyBufPtr for *mut PyByteArrayObject {
    #[inline(always)]
    unsafe fn length(self) -> Py_ssize_t {
        pyo3_ffi::PyByteArray_Size(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn as_mut_ptr(self) -> *mut u8 {
        pyo3_ffi::PyByteArray_AsString(self as *mut PyObject) as *mut u8
    }
}

#[inline(always)]
pub unsafe fn new(length: Py_ssize_t) -> *mut PyByteArrayObject {
    pyo3_ffi::PyByteArray_FromStringAndSize(std::ptr::null(), length) as *mut PyByteArrayObject
}
