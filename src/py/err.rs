use pyo3_ffi::*;
use std::ffi::CStr;
use std::os::raw::c_int;

use super::PyTypeInfo;

#[inline(always)]
pub unsafe fn set<E: PyTypeInfo, T: PyTypeInfo>(exception: *mut E, value: *mut T) {
    pyo3_ffi::PyErr_SetObject(exception as *mut PyObject, value as *mut PyObject)
}

#[inline(always)]
pub unsafe fn set_string<E: PyTypeInfo>(exception: *mut E, message: &CStr) {
    pyo3_ffi::PyErr_SetString(exception as *mut PyObject, message.as_ptr())
}

macro_rules! format {
    ($exception:expr, $format_string:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyErr_Format(
                ($exception) as *mut ::pyo3_ffi::PyObject,
                ($format_string).as_ptr()
                $(, $argument )*
            )
        }
    }};
}

pub(crate) use format;

#[inline(always)]
pub unsafe fn occurred() -> *mut PyObject {
    pyo3_ffi::PyErr_Occurred()
}

#[inline(always)]
pub unsafe fn clear() {
    pyo3_ffi::PyErr_Clear()
}

#[inline(always)]
pub unsafe fn fetch() -> (*mut PyObject, *mut PyObject, *mut PyObject) {
    let mut type_pointer = std::ptr::null_mut();
    let mut value = std::ptr::null_mut();
    let mut traceback = std::ptr::null_mut();
    #[allow(deprecated)]
    unsafe {
        pyo3_ffi::PyErr_Fetch(&mut type_pointer, &mut value, &mut traceback);
    }
    (type_pointer, value, traceback)
}

#[inline(always)]
pub unsafe fn restore(type_pointer: *mut PyObject, value: *mut PyObject, traceback: *mut PyObject) {
    #[allow(deprecated)]
    unsafe {
        pyo3_ffi::PyErr_Restore(type_pointer, value, traceback);
    }
}

#[inline(always)]
pub unsafe fn normalize(
    type_pointer: &mut *mut PyObject,
    value: &mut *mut PyObject,
    traceback: &mut *mut PyObject,
) {
    #[allow(deprecated)]
    unsafe {
        pyo3_ffi::PyErr_NormalizeException(type_pointer, value, traceback);
    }
}

#[inline(always)]
pub unsafe fn matches<E: PyTypeInfo, A: PyTypeInfo>(error: *mut E, against: *mut A) -> bool {
    pyo3_ffi::PyErr_GivenExceptionMatches(error as *mut PyObject, against as *mut PyObject) != 0
}

#[inline(always)]
pub unsafe fn matches_current<A: PyTypeInfo>(against: *mut A) -> bool {
    pyo3_ffi::PyErr_ExceptionMatches(against as *mut PyObject) != 0
}

#[inline(always)]
pub unsafe fn warn<C: PyTypeInfo>(category: *mut C, message: &CStr, stack_level: i32) -> c_int {
    pyo3_ffi::PyErr_WarnEx(
        category as *mut PyObject,
        message.as_ptr(),
        stack_level as Py_ssize_t,
    )
}

#[inline(always)]
pub unsafe fn set_traceback<E: PyTypeInfo, T: PyTypeInfo>(
    exception: *mut E,
    traceback: *mut T,
) -> c_int {
    pyo3_ffi::PyException_SetTraceback(exception as *mut PyObject, traceback as *mut PyObject)
}

#[inline(always)]
pub unsafe fn set_cause<E: PyTypeInfo, C: PyTypeInfo>(exception: *mut E, cause: *mut C) {
    pyo3_ffi::PyException_SetCause(exception as *mut PyObject, cause as *mut PyObject)
}
