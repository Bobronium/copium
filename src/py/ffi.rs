#![allow(dead_code)]
#![allow(non_snake_case)]
#![allow(unused_imports)]
#![allow(unused_unsafe)]

use core::ffi::{c_char, CStr};
use libc::c_ulong;
use pyo3_ffi::*;
use std::os::raw::c_int;

#[cfg(Py_GIL_DISABLED)]
use core::sync::atomic::Ordering;

pub use pyo3_ffi::{_PyWeakref_RefType, PyEllipsis_Type, PyProperty_Type, Py_None};

#[cfg_attr(windows, link(name = "pythonXY"))]
extern "C" {
    pub static mut PyMethod_Type: PyTypeObject;
    pub static mut _PyNone_Type: PyTypeObject;
    pub static mut _PyNotImplemented_Type: PyTypeObject;
    pub static mut _Py_EllipsisObject: PyObject;
    pub static mut _Py_NoneStruct: PyObject;
}

#[repr(C)]
pub struct PyMethodObject {
    pub ob_base: PyObject,
    pub im_func: *mut PyObject,
    pub im_self: *mut PyObject,
    pub im_weakreflist: *mut PyObject,
    pub vectorcall: vectorcallfunc,
}

#[inline(always)]
pub unsafe fn PyMethod_GET_FUNCTION(method_object: *mut PyObject) -> *mut PyObject {
    unsafe { (*(method_object as *mut PyMethodObject)).im_func }
}

#[inline(always)]
pub unsafe fn PyMethod_GET_SELF(method_object: *mut PyObject) -> *mut PyObject {
    unsafe { (*(method_object as *mut PyMethodObject)).im_self }
}

#[cfg_attr(windows, link(name = "pythonXY"))]
extern "C" {
    pub fn PyMethod_New(function: *mut PyObject, self_object: *mut PyObject) -> *mut PyObject;
}

#[cfg_attr(windows, link(name = "pythonXY"))]
extern "C" {
    pub fn PyErr_Format(exception: *mut PyObject, format: *const c_char, ...) -> *mut PyObject;
    pub fn PyUnicode_FromFormat(format: *const c_char, ...) -> *mut PyObject;
    pub fn PySequence_Fast(object: *mut PyObject, message: *const c_char) -> *mut PyObject;
    pub fn PyObject_CallFunction(
        callable: *mut PyObject,
        format: *const c_char,
        ...
    ) -> *mut PyObject;
    pub fn PyObject_CallFunctionObjArgs(callable: *mut PyObject, ...) -> *mut PyObject;
    pub fn PyObject_CallMethodObjArgs(
        callable: *mut PyObject,
        name: *mut PyObject,
        ...
    ) -> *mut PyObject;
    #[cfg(Py_3_12)]
    pub fn PyFunction_SetVectorcall(callable: *mut PyObject, vectorcall: vectorcallfunc);
    pub fn PyVectorcall_Function(callable: *mut PyObject) -> Option<vectorcallfunc>;
}

extern "C" {
    pub fn _PyDict_NewPresized(minused: Py_ssize_t) -> *mut PyObject;
    pub fn _PySet_NextEntry(
        set: *mut PyObject,
        pos: *mut Py_ssize_t,
        key: *mut *mut PyObject,
        hash: *mut Py_hash_t,
    ) -> c_int;
}

#[cfg(Py_3_13)]
extern "C" {
    pub fn _PyDict_SetItem_Take2(
        dictionary: *mut PyObject,
        key: *mut PyObject,
        value: *mut PyObject,
    ) -> c_int;
}

#[cfg(not(Py_3_13))]
#[inline(always)]
pub unsafe fn _PyDict_SetItem_Take2(
    dictionary: *mut PyObject,
    key: *mut PyObject,
    value: *mut PyObject,
) -> c_int {
    unsafe {
        let status = PyDict_SetItem(dictionary, key, value);
        Py_DECREF(key);
        Py_DECREF(value);
        status
    }
}

#[cfg(any(Py_3_13, Py_3_14))]
extern "C" {
    pub fn PyObject_GetOptionalAttr(
        object: *mut PyObject,
        name: *mut PyObject,
        result: *mut *mut PyObject,
    ) -> c_int;
}

#[cfg(not(any(Py_3_13, Py_3_14)))]
extern "C" {
    fn _PyObject_LookupAttr(
        object: *mut PyObject,
        name: *mut PyObject,
        result: *mut *mut PyObject,
    ) -> c_int;
}

#[cfg(not(any(Py_3_13, Py_3_14)))]
#[inline(always)]
pub unsafe fn PyObject_GetOptionalAttr(
    object: *mut PyObject,
    name: *mut PyObject,
    result: *mut *mut PyObject,
) -> c_int {
    unsafe { _PyObject_LookupAttr(object, name, result) }
}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
extern "C" {
    pub fn PyDict_AddWatcher(
        callback: Option<
            unsafe extern "C" fn(
                event: i32,
                dict: *mut PyObject,
                key: *mut PyObject,
                new_value: *mut PyObject,
            ) -> i32,
        >,
    ) -> i32;
    pub fn PyDict_ClearWatcher(watcher_id: i32) -> i32;
    pub fn PyDict_Watch(watcher_id: i32, dict: *mut PyObject) -> i32;
    pub fn PyDict_Unwatch(watcher_id: i32, dict: *mut PyObject) -> i32;
}

#[cfg(all(Py_3_14, Py_GIL_DISABLED))]
extern "C" {
    pub fn PyIter_NextItem(iterator: *mut PyObject, item: *mut *mut PyObject) -> i32;
}

#[inline(always)]
pub unsafe fn PySequence_Fast_GET_SIZE(object: *mut PyObject) -> Py_ssize_t {
    unsafe { Py_SIZE(object) }
}

#[cfg(Py_GIL_DISABLED)]
#[inline(always)]
pub unsafe fn tp_flags_of(type_object: *mut PyTypeObject) -> c_ulong {
    unsafe { (*type_object).tp_flags.load(Ordering::Relaxed) }
}

#[cfg(not(Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn tp_flags_of(type_object: *mut PyTypeObject) -> c_ulong {
    unsafe { (*type_object).tp_flags }
}

#[inline(always)]
pub unsafe fn PySequence_Fast_GET_ITEM(
    object: *mut PyObject,
    index: Py_ssize_t,
) -> *mut PyObject {
    unsafe {
        if (tp_flags_of((*object).ob_type) & (Py_TPFLAGS_LIST_SUBCLASS as c_ulong)) != 0 {
            *(*(object as *mut PyListObject)).ob_item.add(index as usize)
        } else {
            *(*(object as *mut PyTupleObject))
                .ob_item
                .as_ptr()
                .add(index as usize)
        }
    }
}

#[inline(always)]
pub fn PyVectorcall_NARGS(nargsf: usize) -> Py_ssize_t {
    const VECTORCALL_OFFSET_BIT: usize = 1usize << (usize::BITS - 1);
    (nargsf & !VECTORCALL_OFFSET_BIT) as Py_ssize_t
}

#[macro_export]
macro_rules! cstr {
    ($string_literal:literal) => {{
        #[allow(unused_unsafe)]
        unsafe {
            ::std::ffi::CStr::from_bytes_with_nul_unchecked(
                concat!($string_literal, "\0").as_bytes(),
            )
        }
    }};
}

#[inline(always)]
pub fn ptr_from_cstr(value: &CStr) -> *const c_char {
    value.as_ptr()
}
