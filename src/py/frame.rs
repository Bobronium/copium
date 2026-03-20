use pyo3_ffi::*;

pub unsafe trait PyFramePtr {
    unsafe fn back(self) -> *mut PyFrameObject;
    unsafe fn code(self) -> *mut PyCodeObject;
    unsafe fn line_number(self) -> i32;
}

unsafe impl PyFramePtr for *mut PyFrameObject {
    #[inline(always)]
    unsafe fn back(self) -> *mut PyFrameObject {
        pyo3_ffi::PyFrame_GetBack(self)
    }

    #[inline(always)]
    unsafe fn code(self) -> *mut PyCodeObject {
        pyo3_ffi::PyFrame_GetCode(self)
    }

    #[inline(always)]
    unsafe fn line_number(self) -> i32 {
        pyo3_ffi::PyFrame_GetLineNumber(self)
    }
}
