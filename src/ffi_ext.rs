#![allow(non_snake_case)]
use core::ffi::c_char;
use libc::c_ulong;
use pyo3_ffi::*;

use crate::types::PyObjectPtr;

#[cfg(Py_GIL_DISABLED)]
use core::sync::atomic::Ordering;

// ── Symbols not (reliably) in pyo3-ffi ──────────────────────

extern "C" {
    pub static mut _Py_NoneStruct: PyObject;
    pub static mut _Py_NotImplementedStruct: PyObject;
    pub static mut _Py_EllipsisObject: PyObject;
    pub static mut PyMethod_Type: PyTypeObject;
    pub static mut _PyNone_Type: PyTypeObject;
    pub static mut _PyNotImplemented_Type: PyTypeObject;
    pub static mut PyEllipsis_Type: PyTypeObject;
    pub static mut PyProperty_Type: PyTypeObject;
    pub static mut _PyWeakref_RefType: PyTypeObject;
}

/// PyMethod_Function is a macro in CPython; access via struct layout.
#[repr(C)]
pub struct PyMethodObject {
    pub ob_base: PyObject,
    pub im_func: *mut PyObject,
    pub im_self: *mut PyObject,
    pub im_weakreflist: *mut PyObject,
    pub vectorcall: vectorcallfunc,
}

#[inline(always)]
pub unsafe fn PyMethod_GET_FUNCTION(m: *mut PyObject) -> *mut PyObject {
    unsafe { (*(m as *mut PyMethodObject)).im_func }
}

#[inline(always)]
pub unsafe fn PyMethod_GET_SELF(m: *mut PyObject) -> *mut PyObject {
    unsafe { (*(m as *mut PyMethodObject)).im_self }
}

extern "C" {
    pub fn PyMethod_New(func: *mut PyObject, self_: *mut PyObject) -> *mut PyObject;
}

#[inline(always)]
pub unsafe fn Py_NotImplemented() -> *mut PyObject {
    std::ptr::addr_of_mut!(_Py_NotImplementedStruct)
}

#[inline(always)]
pub unsafe fn Py_None() -> *mut PyObject {
    std::ptr::addr_of_mut!(_Py_NoneStruct)
}

// ── Variadic FFI not reliably in pyo3-ffi ───────────────────

extern "C" {
    pub fn PyErr_Format(exception: *mut PyObject, format: *const c_char, ...) -> *mut PyObject;
    pub fn PyUnicode_FromFormat(format: *const c_char, ...) -> *mut PyObject;
    pub fn PySequence_Fast(o: *mut PyObject, m: *const c_char) -> *mut PyObject;
    #[cfg(Py_3_12)]
    pub fn PyFunction_SetVectorcall(callable: *mut PyObject, vc: vectorcallfunc);
    pub fn PyVectorcall_Function(callable: *mut PyObject) -> Option<vectorcallfunc>;
}

#[inline(always)]
pub unsafe fn PySequence_Fast_GET_SIZE(o: *mut PyObject) -> Py_ssize_t {
    unsafe { Py_SIZE(o) }
}

#[cfg(Py_GIL_DISABLED)]
#[inline(always)]
pub unsafe fn tp_flags_of(tp: *mut PyTypeObject) -> c_ulong {
    unsafe { (*tp).tp_flags.load(Ordering::Relaxed) }
}

#[cfg(not(Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn tp_flags_of(tp: *mut PyTypeObject) -> c_ulong {
    unsafe { (*tp).tp_flags }
}

#[inline(always)]
pub unsafe fn PySequence_Fast_GET_ITEM(o: *mut PyObject, i: Py_ssize_t) -> *mut PyObject {
    unsafe {
        if (tp_flags_of(o.class()) & (Py_TPFLAGS_LIST_SUBCLASS as c_ulong)) != 0 {
            *(*(o as *mut PyListObject)).ob_item.add(i as usize)
        } else {
            *(*(o as *mut PyTupleObject))
                .ob_item
                .as_ptr()
                .add(i as usize)
        }
    }
}

/// Strips PY_VECTORCALL_ARGUMENTS_OFFSET from nargsf.
#[inline(always)]
pub fn PyVectorcall_NARGS(nargsf: usize) -> Py_ssize_t {
    const OFFSET_BIT: usize = 1usize << (usize::BITS - 1);
    (nargsf & !OFFSET_BIT) as Py_ssize_t
}

// ── C string literal helper ─────────────────────────────────

#[macro_export]
macro_rules! cstr {
    ($s:literal) => {
        concat!($s, "\0").as_ptr() as *const core::ffi::c_char
    };
}
