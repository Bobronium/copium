use pyo3_ffi::*;
use std::ffi::CStr;
use std::os::raw::c_int;

use super::PyTypeInfo;

pub unsafe trait PyUnicodePtr {
    unsafe fn as_utf8<'a>(self) -> &'a CStr;
    unsafe fn byte_length(self) -> Py_ssize_t;
    unsafe fn compare_ascii(self, string: &CStr) -> i32;
    unsafe fn join<Sequence: PyTypeInfo>(self, sequence: *mut Sequence) -> *mut PyUnicodeObject;
    unsafe fn tailmatch<S: PyTypeInfo>(
        self,
        substring: *mut S,
        start: Py_ssize_t,
        end: Py_ssize_t,
        direction: i32,
    ) -> bool;
}

unsafe impl<T: PyTypeInfo> PyUnicodePtr for *mut T {
    #[inline(always)]
    unsafe fn as_utf8<'a>(self) -> &'a CStr {
        CStr::from_ptr(pyo3_ffi::PyUnicode_AsUTF8(self as *mut PyObject))
    }

    #[inline(always)]
    unsafe fn byte_length(self) -> Py_ssize_t {
        pyo3_ffi::PyUnicode_GET_LENGTH(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn compare_ascii(self, string: &CStr) -> i32 {
        pyo3_ffi::PyUnicode_CompareWithASCIIString(self as *mut PyObject, string.as_ptr())
    }

    #[inline(always)]
    unsafe fn join<Sequence: PyTypeInfo>(self, sequence: *mut Sequence) -> *mut PyUnicodeObject {
        pyo3_ffi::PyUnicode_Join(self as *mut PyObject, sequence as *mut PyObject)
            as *mut PyUnicodeObject
    }

    #[inline(always)]
    unsafe fn tailmatch<S: PyTypeInfo>(
        self,
        substring: *mut S,
        start: Py_ssize_t,
        end: Py_ssize_t,
        direction: i32,
    ) -> bool {
        pyo3_ffi::PyUnicode_Tailmatch(
            self as *mut PyObject,
            substring as *mut PyObject,
            start,
            end,
            direction as c_int,
        ) != 0
    }
}

#[inline(always)]
pub unsafe fn from_cstr(value: &CStr) -> *mut PyUnicodeObject {
    pyo3_ffi::PyUnicode_FromString(value.as_ptr()) as *mut PyUnicodeObject
}

#[inline(always)]
pub unsafe fn from_str_and_size(value: &str) -> *mut PyUnicodeObject {
    pyo3_ffi::PyUnicode_FromStringAndSize(value.as_ptr().cast(), value.len() as Py_ssize_t)
        as *mut PyUnicodeObject
}

macro_rules! from_format {
    ($format_string:expr $(, $argument:expr )* $(,)?) => {{
        #[allow(unused_unsafe)]
        unsafe {
            $crate::py::ffi::PyUnicode_FromFormat(
                ($format_string).as_ptr()
                $(, $argument )*
            ) as *mut $crate::py::PyUnicodeObject
        }
    }};
}

pub(crate) use from_format;

#[inline(always)]
pub unsafe fn intern(value: &CStr) -> *mut PyUnicodeObject {
    pyo3_ffi::PyUnicode_InternFromString(value.as_ptr()) as *mut PyUnicodeObject
}
