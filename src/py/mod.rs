#![allow(dead_code)]
#![allow(non_snake_case)]
#![allow(unused_imports)]
#![allow(unused_unsafe)]

use crate::memo::{Memo_Type, PyMemoObject};
use std::ptr::addr_of_mut;

pub mod boolean;
pub mod bytearray;
pub mod call;
pub mod capsule;
pub mod critical_section;
pub mod dict;
pub mod err;
pub mod eval;
pub mod ffi;
pub mod frame;
pub mod gc;
pub mod list;
pub mod long;
pub mod method;
pub mod module;
pub mod object;
pub mod seq;
pub mod set;
pub mod tuple;
pub mod type_object;
pub mod unicode;
pub mod vectorcall;

pub use ffi::*;
pub use bytearray::PyBufPtr;
pub use capsule::PyCapsulePtr;
pub use dict::PyMapPtr;
pub use frame::PyFramePtr;
pub use list::PyMutSeqPtr;
pub use method::PyBoundMethodPtr;
pub use object::{PyObjectPtr, PyObjectSlotPtr};
pub use seq::PySeqPtr;
pub use set::{PyMutSetPtr, PySetPtr};
pub use type_object::PyTypeObjectPtr;
pub use unicode::PyUnicodePtr;
pub use vectorcall::PyVectorcallPtr;

pub unsafe trait PyTypeInfo: Sized {
    fn type_ptr() -> *mut PyTypeObject;

    #[inline(always)]
    fn is(type_pointer: *mut PyTypeObject) -> bool {
        type_pointer == Self::type_ptr()
    }

    #[inline(always)]
    unsafe fn cast_exact(object: *mut PyObject, class: *mut PyTypeObject) -> Option<*mut Self> {
        if Self::is(class) {
            Some(object as *mut Self)
        } else {
            None
        }
    }

    #[inline(always)]
    unsafe fn cast_unchecked(object: *mut PyObject) -> *mut Self {
        object as *mut Self
    }
}

macro_rules! pytype {
    ($($rust_type:ty => $ffi_type:path),+ $(,)?) => {$(
        unsafe impl PyTypeInfo for $rust_type {
            #[inline(always)]
            fn type_ptr() -> *mut PyTypeObject {
                std::ptr::addr_of_mut!($ffi_type)
            }
        }
    )+}
}

pub struct PyFrozensetObject;

pytype! {
    PyObject => PyBaseObject_Type,
    PyUnicodeObject => PyUnicode_Type,
    PyListObject => PyList_Type,
    PyTupleObject => PyTuple_Type,
    PyDictObject => PyDict_Type,
    PySetObject => PySet_Type,
    PyByteArrayObject => PyByteArray_Type,
    PyFrozensetObject => PyFrozenSet_Type,
    ffi::PyMethodObject => ffi::PyMethod_Type,
    PyMemoObject => Memo_Type,
    PySliceObject => PySlice_Type,
    PyLongObject => PyLong_Type,
    PyCodeObject => PyCode_Type,
}

#[allow(non_upper_case_globals)]
pub const NoneObject: *mut PyObject = unsafe { &raw const _Py_NoneStruct as *const _ as *mut _ };
#[allow(non_upper_case_globals)]
pub const EllipsisObject: *mut PyObject = unsafe { &raw const _Py_EllipsisObject as *const _ as *mut _ };
