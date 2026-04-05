use pyo3_ffi::*;

#[inline(always)]
pub unsafe fn from_bool(value: bool) -> *mut PyObject {
    pyo3_ffi::PyBool_FromLong(value as libc::c_long)
}
