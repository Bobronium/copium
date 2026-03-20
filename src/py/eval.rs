use pyo3_ffi::*;
use std::ffi::CStr;

use super::PyTypeInfo;

#[inline(always)]
pub unsafe fn builtins() -> *mut PyDictObject {
    pyo3_ffi::PyEval_GetBuiltins() as *mut PyDictObject
}

#[inline(always)]
pub unsafe fn run_string<G: PyTypeInfo, L: PyTypeInfo>(
    code: &CStr,
    start: i32,
    globals: *mut G,
    locals: *mut L,
) -> *mut PyObject {
    pyo3_ffi::PyRun_StringFlags(
        code.as_ptr(),
        start,
        globals as *mut PyObject,
        locals as *mut PyObject,
        std::ptr::null_mut(),
    )
}

#[inline(always)]
pub unsafe fn current_frame() -> *mut PyFrameObject {
    pyo3_ffi::PyEval_GetFrame()
}
