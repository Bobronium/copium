use super::{ffi, PyTypeInfo};
use crate::py;
use libc::c_ulong;
use pyo3_ffi::*;
use std::ffi::{c_void, CStr};
use std::os::raw::c_int;
use std::ptr;

pub unsafe trait PyObjectPtr: Sized {
    unsafe fn id(self) -> *mut PyLongObject;
    unsafe fn as_object(self) -> *mut PyObject;
    unsafe fn class(self) -> *mut PyTypeObject;
    unsafe fn refcount(self) -> Py_ssize_t;
    unsafe fn incref(self);
    unsafe fn decref(self);
    unsafe fn decref_if_nonnull(self);
    unsafe fn newref(self) -> *mut PyObject;

    unsafe fn getattr<N: PyTypeInfo>(self, name: *mut N) -> *mut PyObject;
    unsafe fn getattr_cstr(self, name: &CStr) -> *mut PyObject;
    unsafe fn getattr_opt<N: PyTypeInfo>(self, name: *mut N, result: &mut *mut PyObject) -> c_int;
    unsafe fn set_attr<N: PyTypeInfo, V: PyTypeInfo>(self, name: *mut N, value: *mut V) -> c_int;
    unsafe fn set_attr_cstr<V: PyTypeInfo>(self, name: &CStr, value: *mut V) -> c_int;
    unsafe fn del_attr<N: PyTypeInfo>(self, name: *mut N) -> c_int;
    unsafe fn del_attr_cstr(self, name: &CStr) -> c_int;
    unsafe fn has_attr_cstr(self, name: &CStr) -> bool;

    unsafe fn call(self) -> *mut PyObject;
    unsafe fn call_one<A: PyTypeInfo>(self, argument: *mut A) -> *mut PyObject;
    unsafe fn call_with<A: PyTypeInfo>(self, args: *mut A) -> *mut PyObject;
    unsafe fn call_with_kwargs<A: PyTypeInfo, K: PyTypeInfo>(
        self,
        args: *mut A,
        kwargs: *mut K,
    ) -> *mut PyObject;

    unsafe fn setitem<K: PyTypeInfo, V: PyTypeInfo>(self, key: *mut K, value: *mut V) -> c_int;
    unsafe fn getitem<K: PyTypeInfo>(self, key: *mut K) -> *mut PyObject;
    unsafe fn get_iter(self) -> *mut PyObject;
    unsafe fn iter_next(self) -> *mut PyObject;
    unsafe fn fast_sequence(self, message: &CStr) -> *mut PyObject;
    unsafe fn fast_sequence_item_unchecked(self, index: Py_ssize_t) -> *mut PyObject;
    unsafe fn fast_sequence_length(self) -> Py_ssize_t;
    unsafe fn sequence_to_tuple(self) -> *mut PyTupleObject;
    unsafe fn sequence_to_list(self) -> *mut PyListObject;
    #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
    unsafe fn iter_next_item(self, item: &mut *mut PyObject) -> i32;

    unsafe fn repr(self) -> *mut PyUnicodeObject;
    unsafe fn str_(self) -> *mut PyUnicodeObject;
    unsafe fn is_callable(self) -> bool;

    unsafe fn is_type(self) -> bool;
    unsafe fn is_tuple(self) -> bool;
    unsafe fn is_list(self) -> bool;
    unsafe fn is_dict(self) -> bool;
    unsafe fn is_long(self) -> bool;
    unsafe fn is_unicode(self) -> bool;
    unsafe fn is_bytes(self) -> bool;
    unsafe fn is_none(self) -> bool;

    #[inline(always)]
    unsafe fn decref_nullable(self) {
        self.decref_if_nonnull()
    }

    #[inline(always)]
    unsafe fn get_optional_attr<N: PyTypeInfo>(
        self,
        name: *mut N,
        result: &mut *mut PyObject,
    ) -> c_int {
        self.getattr_opt(name, result)
    }
}

unsafe impl<T: PyTypeInfo> PyObjectPtr for *mut T {
    #[inline(always)]
    unsafe fn id(self) -> *mut PyLongObject {
        pyo3_ffi::PyLong_FromVoidPtr(self as *mut c_void) as *mut PyLongObject
    }

    #[inline(always)]
    unsafe fn as_object(self) -> *mut PyObject {
        self as *mut PyObject
    }

    #[inline(always)]
    unsafe fn class(self) -> *mut PyTypeObject {
        (*(self as *mut PyObject)).ob_type
    }

    #[inline(always)]
    unsafe fn refcount(self) -> Py_ssize_t {
        pyo3_ffi::Py_REFCNT(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn incref(self) {
        pyo3_ffi::Py_INCREF(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn decref(self) {
        pyo3_ffi::Py_DECREF(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn decref_if_nonnull(self) {
        if !self.is_null() {
            self.decref();
        }
    }

    #[inline(always)]
    unsafe fn newref(self) -> *mut PyObject {
        pyo3_ffi::Py_NewRef(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn getattr<N: PyTypeInfo>(self, name: *mut N) -> *mut PyObject {
        pyo3_ffi::PyObject_GetAttr(self as *mut PyObject, name as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn getattr_cstr(self, name: &CStr) -> *mut PyObject {
        pyo3_ffi::PyObject_GetAttrString(self as *mut PyObject, name.as_ptr())
    }

    #[inline(always)]
    unsafe fn getattr_opt<N: PyTypeInfo>(self, name: *mut N, result: &mut *mut PyObject) -> c_int {
        ffi::PyObject_GetOptionalAttr(self as *mut PyObject, name as *mut PyObject, result)
    }

    #[inline(always)]
    unsafe fn set_attr<N: PyTypeInfo, V: PyTypeInfo>(self, name: *mut N, value: *mut V) -> c_int {
        pyo3_ffi::PyObject_SetAttr(
            self as *mut PyObject,
            name as *mut PyObject,
            value as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn set_attr_cstr<V: PyTypeInfo>(self, name: &CStr, value: *mut V) -> c_int {
        pyo3_ffi::PyObject_SetAttrString(
            self as *mut PyObject,
            name.as_ptr(),
            value as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn del_attr<N: PyTypeInfo>(self, name: *mut N) -> c_int {
        pyo3_ffi::PyObject_SetAttr(
            self as *mut PyObject,
            name as *mut PyObject,
            ptr::null_mut(),
        )
    }

    #[inline(always)]
    unsafe fn del_attr_cstr(self, name: &CStr) -> c_int {
        pyo3_ffi::PyObject_DelAttrString(self as *mut PyObject, name.as_ptr())
    }

    #[inline(always)]
    unsafe fn has_attr_cstr(self, name: &CStr) -> bool {
        pyo3_ffi::PyObject_HasAttrString(self as *mut PyObject, name.as_ptr()) != 0
    }

    #[inline(always)]
    unsafe fn call(self) -> *mut PyObject {
        pyo3_ffi::PyObject_CallNoArgs(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn call_one<A: PyTypeInfo>(self, argument: *mut A) -> *mut PyObject {
        pyo3_ffi::PyObject_CallOneArg(self as *mut PyObject, argument as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn call_with<A: PyTypeInfo>(self, args: *mut A) -> *mut PyObject {
        pyo3_ffi::PyObject_CallObject(self as *mut PyObject, args as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn call_with_kwargs<A: PyTypeInfo, K: PyTypeInfo>(
        self,
        args: *mut A,
        kwargs: *mut K,
    ) -> *mut PyObject {
        pyo3_ffi::PyObject_Call(
            self as *mut PyObject,
            args as *mut PyObject,
            kwargs as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn setitem<K: PyTypeInfo, V: PyTypeInfo>(self, key: *mut K, value: *mut V) -> c_int {
        pyo3_ffi::PyObject_SetItem(
            self as *mut PyObject,
            key as *mut PyObject,
            value as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn getitem<K: PyTypeInfo>(self, key: *mut K) -> *mut PyObject {
        pyo3_ffi::PyObject_GetItem(self as *mut PyObject, key as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn get_iter(self) -> *mut PyObject {
        pyo3_ffi::PyObject_GetIter(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn iter_next(self) -> *mut PyObject {
        pyo3_ffi::PyIter_Next(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn fast_sequence(self, message: &CStr) -> *mut PyObject {
        ffi::PySequence_Fast(self as *mut PyObject, message.as_ptr())
    }

    #[inline(always)]
    unsafe fn fast_sequence_item_unchecked(self, index: Py_ssize_t) -> *mut PyObject {
        ffi::PySequence_Fast_GET_ITEM(self as *mut PyObject, index)
    }

    #[inline(always)]
    unsafe fn fast_sequence_length(self) -> Py_ssize_t {
        ffi::PySequence_Fast_GET_SIZE(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn sequence_to_tuple(self) -> *mut PyTupleObject {
        pyo3_ffi::PySequence_Tuple(self as *mut PyObject) as *mut PyTupleObject
    }

    #[inline(always)]
    unsafe fn sequence_to_list(self) -> *mut PyListObject {
        pyo3_ffi::PySequence_List(self as *mut PyObject) as *mut PyListObject
    }

    #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
    #[inline(always)]
    unsafe fn iter_next_item(self, item: &mut *mut PyObject) -> i32 {
        ffi::PyIter_NextItem(self as *mut PyObject, item)
    }

    #[inline(always)]
    unsafe fn repr(self) -> *mut PyUnicodeObject {
        pyo3_ffi::PyObject_Repr(self as *mut PyObject) as *mut PyUnicodeObject
    }

    #[inline(always)]
    unsafe fn str_(self) -> *mut PyUnicodeObject {
        pyo3_ffi::PyObject_Str(self as *mut PyObject) as *mut PyUnicodeObject
    }

    #[inline(always)]
    unsafe fn is_callable(self) -> bool {
        pyo3_ffi::PyCallable_Check(self as *mut PyObject) != 0
    }

    #[inline(always)]
    unsafe fn is_type(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_TYPE_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_tuple(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_TUPLE_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_list(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_LIST_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_dict(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_DICT_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_long(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_LONG_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_unicode(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_UNICODE_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_bytes(self) -> bool {
        (ffi::tp_flags_of(self.class()) & (Py_TPFLAGS_BYTES_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_none(self) -> bool {
        self as *mut PyObject == py::NoneObject
    }
}

#[inline(always)]
pub unsafe fn getattr_cstr<O: PyTypeInfo>(object: *mut O, name: &CStr) -> *mut PyObject {
    object.getattr_cstr(name)
}

#[inline(always)]
pub unsafe fn set_attr_cstr<O: PyTypeInfo, V: PyTypeInfo>(
    object: *mut O,
    name: &CStr,
    value: *mut V,
) -> c_int {
    object.set_attr_cstr(name, value)
}

#[inline(always)]
pub unsafe fn del_attr_cstr<O: PyTypeInfo>(object: *mut O, name: &CStr) -> c_int {
    object.del_attr_cstr(name)
}

#[inline(always)]
pub unsafe fn has_attr_cstr<O: PyTypeInfo>(object: *mut O, name: &CStr) -> bool {
    object.has_attr_cstr(name)
}

#[inline(always)]
pub unsafe fn call_no_args<O: PyTypeInfo>(object: *mut O) -> *mut PyObject {
    object.call()
}

#[inline(always)]
pub unsafe fn call_one_arg<O: PyTypeInfo, A: PyTypeInfo>(
    object: *mut O,
    argument: *mut A,
) -> *mut PyObject {
    object.call_one(argument)
}

#[inline(always)]
pub unsafe fn call_with<O: PyTypeInfo, A: PyTypeInfo>(
    object: *mut O,
    args: *mut A,
) -> *mut PyObject {
    object.call_with(args)
}

#[inline(always)]
pub unsafe fn call_with_kwargs<O: PyTypeInfo, A: PyTypeInfo, K: PyTypeInfo>(
    object: *mut O,
    args: *mut A,
    kwargs: *mut K,
) -> *mut PyObject {
    object.call_with_kwargs(args, kwargs)
}

#[inline(always)]
pub unsafe fn get_iter<O: PyTypeInfo>(object: *mut O) -> *mut PyObject {
    object.get_iter()
}

#[cfg(all(Py_3_14, Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn iter_next_item<I: PyTypeInfo>(iterator: *mut I, item: &mut *mut PyObject) -> i32 {
    iterator.iter_next_item(item)
}

#[inline(always)]
pub unsafe fn repr<O: PyTypeInfo>(object: *mut O) -> *mut PyUnicodeObject {
    object.repr()
}

#[inline(always)]
pub unsafe fn str_<O: PyTypeInfo>(object: *mut O) -> *mut PyUnicodeObject {
    object.str_()
}

pub unsafe trait PyObjectSlotPtr {
    unsafe fn clear(self);
}

unsafe impl<T: PyTypeInfo> PyObjectSlotPtr for *mut *mut T {
    #[inline(always)]
    unsafe fn clear(self) {
        if self.is_null() {
            return;
        }

        let old_value = *self;
        *self = ptr::null_mut();
        old_value.decref_if_nonnull();
    }
}
