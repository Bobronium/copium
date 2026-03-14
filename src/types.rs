use libc::c_ulong;
use pyo3_ffi::*;
use std::hint::unlikely;
use std::os::raw::{c_char, c_int};
use std::ptr;

use crate::compat;
use crate::ffi_ext;
use crate::ffi_ext::*;
use crate::memo::{Memo_Type, PyMemoObject};
use crate::state::STATE;

// ── Type identity (on the type, not the pointer) ───────────

pub unsafe trait PyTypeInfo: Sized {
    fn type_ptr() -> *mut PyTypeObject;
    #[inline(always)]
    fn is(tp: *mut PyTypeObject) -> bool {
        tp == Self::type_ptr()
    }

    #[inline(always)]
    unsafe fn cast_exact(object: *mut PyObject, cls: *mut PyTypeObject) -> Option<*mut Self> {
        if Self::is(cls) {
            Some(object as *mut Self)
        } else {
            None
        }
    }
}

macro_rules! pytype {
    ($($rust:ty => $ffi:ident),+ $(,)?) => {$(
        unsafe impl PyTypeInfo for $rust {
            #[inline(always)]
            fn type_ptr() -> *mut PyTypeObject {
                std::ptr::addr_of_mut!($ffi)
            }
        }
    )+}
}
pub struct PyFrozensetObject;

pytype! {
    PyObject          => PyBaseObject_Type,
    PyListObject      => PyList_Type,
    PyTupleObject     => PyTuple_Type,
    PyDictObject      => PyDict_Type,
    PySetObject       => PySet_Type,
    PyByteArrayObject => PyByteArray_Type,
    PyFrozensetObject => PyFrozenSet_Type,
    PyMethodObject    => PyMethod_Type,
    PyMemoObject      => Memo_Type,
}

// ── PyAnyPtr — pointer methods on every *mut T ─────────────

pub unsafe trait PyObjectPtr {
    unsafe fn refcount(self) -> Py_ssize_t;
    unsafe fn incref(self);
    unsafe fn decref(self);
    unsafe fn decref_nullable(self);
    unsafe fn newref(self) -> *mut PyObject;
    unsafe fn class(self) -> *mut PyTypeObject;
    unsafe fn getattr(self, name: *mut PyObject) -> *mut PyObject;
    unsafe fn get_optional_attr(self, name: *mut PyObject, out: &mut *mut PyObject) -> c_int;
    unsafe fn call(self) -> *mut PyObject;
    unsafe fn call_one(self, arg: *mut PyObject) -> *mut PyObject;
    unsafe fn call_with(self, args: *mut PyObject) -> *mut PyObject;
    unsafe fn set_attr(self, name: *mut PyObject, value: *mut PyObject) -> c_int;
    unsafe fn get_iter(self) -> *mut PyObject;

    unsafe fn is_type(self) -> bool;
    unsafe fn is_tuple(self) -> bool;
    unsafe fn is_dict(self) -> bool;
    unsafe fn is_unicode(self) -> bool;
    unsafe fn is_bytes(self) -> bool;
    unsafe fn is_none(self) -> bool;
}

unsafe impl<T: PyTypeInfo> PyObjectPtr for *mut T {
    #[inline(always)]
    unsafe fn refcount(self) -> Py_ssize_t {
        Py_REFCNT(self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn incref(self) {
        Py_INCREF(self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn decref(self) {
        Py_DECREF(self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn decref_nullable(self) {
        if !self.is_null() {
            self.decref();
        }
    }
    #[inline(always)]
    unsafe fn newref(self) -> *mut PyObject {
        Py_NewRef(self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn class(self) -> *mut PyTypeObject {
        (*(self as *mut PyObject)).ob_type
    }
    #[inline(always)]
    unsafe fn getattr(self, name: *mut PyObject) -> *mut PyObject {
        PyObject_GetAttr(self as *mut PyObject, name)
    }
    #[inline(always)]
    unsafe fn get_optional_attr(self, name: *mut PyObject, out: &mut *mut PyObject) -> c_int {
        compat::PyObject_GetOptionalAttr(self as *mut PyObject, name, out)
    }
    #[inline(always)]
    unsafe fn call(self) -> *mut PyObject {
        PyObject_CallNoArgs(self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn call_one(self, arg: *mut PyObject) -> *mut PyObject {
        PyObject_CallOneArg(self as *mut PyObject, arg)
    }
    #[inline(always)]
    unsafe fn call_with(self, args: *mut PyObject) -> *mut PyObject {
        PyObject_CallObject(self as *mut PyObject, args)
    }
    #[inline(always)]
    unsafe fn set_attr(self, name: *mut PyObject, value: *mut PyObject) -> c_int {
        PyObject_SetAttr(self as *mut PyObject, name, value)
    }
    #[inline(always)]
    unsafe fn get_iter(self) -> *mut PyObject {
        PyObject_GetIter(self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn is_type(self) -> bool {
        (crate::ffi_ext::tp_flags_of(self.class()) & (Py_TPFLAGS_TYPE_SUBCLASS as c_ulong)) != 0
    }
    #[inline(always)]
    unsafe fn is_tuple(self) -> bool {
        (crate::ffi_ext::tp_flags_of(self.class()) & (Py_TPFLAGS_TUPLE_SUBCLASS as c_ulong)) != 0
    }
    #[inline(always)]
    unsafe fn is_dict(self) -> bool {
        (crate::ffi_ext::tp_flags_of(self.class()) & (Py_TPFLAGS_DICT_SUBCLASS as c_ulong)) != 0
    }
    #[inline(always)]
    unsafe fn is_unicode(self) -> bool {
        (crate::ffi_ext::tp_flags_of(self.class()) & (Py_TPFLAGS_UNICODE_SUBCLASS as c_ulong)) != 0
    }
    #[inline(always)]
    unsafe fn is_bytes(self) -> bool {
        (crate::ffi_ext::tp_flags_of(self.class()) & (Py_TPFLAGS_BYTES_SUBCLASS as c_ulong)) != 0
    }
    #[inline(always)]
    unsafe fn is_none(self) -> bool {
        self as *mut PyObject == ffi_ext::Py_None()
    }
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
        old_value.decref_nullable();
    }
}

// ── Constructors ───────────────────────────────────────────

#[inline(always)]
pub unsafe fn py_list_new(n: Py_ssize_t) -> *mut PyListObject {
    PyList_New(n) as _
}

#[inline(always)]
pub unsafe fn py_tuple_new(n: Py_ssize_t) -> *mut PyTupleObject {
    PyTuple_New(n) as _
}

#[inline(always)]
pub unsafe fn py_dict_new(n: Py_ssize_t) -> *mut PyDictObject {
    compat::_PyDict_NewPresized(n) as _
}

#[inline(always)]
pub unsafe fn py_set_new() -> *mut PySetObject {
    PySet_New(ptr::null_mut()) as _
}

#[inline(always)]
pub unsafe fn frozenset_from(iterable: *mut PyObject) -> *mut PyObject {
    PyFrozenSet_New(iterable)
}

#[inline(always)]
pub unsafe fn py_bytearray_new(n: Py_ssize_t) -> *mut PyByteArrayObject {
    PyByteArray_FromStringAndSize(ptr::null(), n) as _
}

#[inline(always)]
pub unsafe fn py_method_new(
    func: *mut PyObject,
    self_: *mut PyObject,
) -> *mut ffi_ext::PyMethodObject {
    ffi_ext::PyMethod_New(func, self_) as _
}

// ── Indexed sequence ops (list, tuple) ─────────────────────
#[inline(always)]
unsafe fn valid_index(i: Py_ssize_t, limit: Py_ssize_t) -> bool {
    (i as usize) < (limit as usize)
}
pub unsafe trait PySeqPtr: Sized {
    unsafe fn length(&self) -> Py_ssize_t;
    unsafe fn get_borrowed_unchecked(self, i: Py_ssize_t) -> *mut PyObject;
    unsafe fn set_slot_steal_unchecked(self, i: Py_ssize_t, v: *mut PyObject);

    #[inline(always)]
    unsafe fn get_owned_check_bounds(self, i: Py_ssize_t) -> *mut PyObject {
        // analogous to _PyList_GetItemRef()
        if unlikely(!valid_index(i, self.length())) {
            return ptr::null_mut();
        }
        self.get_borrowed_unchecked(i).newref()
    }
}

unsafe impl PySeqPtr for *mut PyListObject {
    #[inline(always)]
    unsafe fn length(&self) -> Py_ssize_t {
        PyList_GET_SIZE(*self as _)
    }
    #[inline(always)]
    unsafe fn get_borrowed_unchecked(self, i: Py_ssize_t) -> *mut PyObject {
        PyList_GET_ITEM(self as _, i)
    }
    #[inline(always)]
    unsafe fn set_slot_steal_unchecked(self, i: Py_ssize_t, v: *mut PyObject) {
        PyList_SET_ITEM(self as _, i, v)
    }
}

unsafe impl PySeqPtr for *mut PyTupleObject {
    #[inline(always)]
    unsafe fn length(&self) -> Py_ssize_t {
        PyTuple_GET_SIZE(*self as *mut PyObject)
    }
    #[inline(always)]
    unsafe fn get_borrowed_unchecked(self, i: Py_ssize_t) -> *mut PyObject {
        PyTuple_GET_ITEM(self as _, i)
    }
    #[inline(always)]
    unsafe fn set_slot_steal_unchecked(self, i: Py_ssize_t, v: *mut PyObject) {
        PyTuple_SET_ITEM(self as _, i, v)
    }
}

// ── Mapping ops (dict) ─────────────────────────────────────

pub unsafe trait PyMapPtr {
    unsafe fn len(self) -> Py_ssize_t;
    /// Does NOT steal references.
    unsafe fn set_item(self, k: *mut PyObject, v: *mut PyObject) -> c_int;
    /// Steals both key and value references.
    unsafe fn set_item_steal_two(self, k: *mut PyObject, v: *mut PyObject) -> c_int;
    /// Returns borrowed ref or null.
    unsafe fn get_item(self, k: *mut PyObject) -> *mut PyObject;
}

unsafe impl PyMapPtr for *mut PyDictObject {
    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        PyDict_Size(self as _)
    }
    #[inline(always)]
    unsafe fn set_item(self, k: *mut PyObject, v: *mut PyObject) -> c_int {
        PyDict_SetItem(self as _, k, v)
    }
    #[inline(always)]
    unsafe fn set_item_steal_two(self, k: *mut PyObject, v: *mut PyObject) -> c_int {
        compat::_PyDict_SetItem_Take2(self as _, k, v)
    }
    #[inline(always)]
    unsafe fn get_item(self, k: *mut PyObject) -> *mut PyObject {
        PyDict_GetItemWithError(self as _, k)
    }
}

pub unsafe trait PySetPtr {
    unsafe fn len(self) -> Py_ssize_t;
    unsafe fn next_entry(
        self,
        pos: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        hash: &mut Py_hash_t,
    ) -> c_int;
}

unsafe impl PySetPtr for *mut PySetObject {
    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        PySet_Size(self as _)
    }
    #[inline(always)]
    unsafe fn next_entry(
        self,
        pos: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        hash: &mut Py_hash_t,
    ) -> c_int {
        _PySet_NextEntry(self as _, pos, key, hash)
    }
}

unsafe impl PySetPtr for *mut PyFrozensetObject {
    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        PySet_Size(self as _)
    }
    #[inline(always)]
    unsafe fn next_entry(
        self,
        pos: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        hash: &mut Py_hash_t,
    ) -> c_int {
        _PySet_NextEntry(self as _, pos, key, hash)
    }
}

pub unsafe trait PyMutSetPtr {
    unsafe fn add_item(self, item: *mut PyObject) -> c_int;
}

unsafe impl PyMutSetPtr for *mut PySetObject {
    #[inline(always)]
    unsafe fn add_item(self, item: *mut PyObject) -> c_int {
        PySet_Add(self as _, item)
    }
}

// ── Buffer ops (bytearray) ─────────────────────────────────

pub unsafe trait PyBufPtr {
    unsafe fn len(self) -> Py_ssize_t;
    unsafe fn as_ptr(self) -> *mut c_char;
}

unsafe impl PyBufPtr for *mut PyByteArrayObject {
    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        PyByteArray_Size(self as _)
    }
    #[inline(always)]
    unsafe fn as_ptr(self) -> *mut c_char {
        PyByteArray_AsString(self as _)
    }
}

// ── Bound method ops ───────────────────────────────────────

pub unsafe trait PyBoundMethodPtr {
    unsafe fn function(self) -> *mut PyObject;
    unsafe fn self_obj(self) -> *mut PyObject;
}

unsafe impl PyBoundMethodPtr for *mut ffi_ext::PyMethodObject {
    #[inline(always)]
    unsafe fn function(self) -> *mut PyObject {
        ffi_ext::PyMethod_GET_FUNCTION(self as _)
    }
    #[inline(always)]
    unsafe fn self_obj(self) -> *mut PyObject {
        ffi_ext::PyMethod_GET_SELF(self as _)
    }
}

// ── Immutability checks ────────────────────────────────────

pub unsafe trait PyTypeObjectPtr {
    unsafe fn is_literal_immutable(self) -> bool;
    unsafe fn is_builtin_immutable(self) -> bool;
    unsafe fn is_stdlib_immutable(self) -> bool;
    unsafe fn is_type_subclass(self) -> bool;
    unsafe fn is_atomic_immutable(self) -> bool;
}

unsafe impl PyTypeObjectPtr for *mut PyTypeObject {
    #[inline(always)]
    unsafe fn is_literal_immutable(self) -> bool {
        (self == std::ptr::addr_of_mut!(_PyNone_Type))
            | (self == std::ptr::addr_of_mut!(PyLong_Type))
            | (self == std::ptr::addr_of_mut!(PyUnicode_Type))
            | (self == std::ptr::addr_of_mut!(PyBool_Type))
            | (self == std::ptr::addr_of_mut!(PyFloat_Type))
            | (self == std::ptr::addr_of_mut!(PyBytes_Type))
    }

    #[inline(always)]
    unsafe fn is_builtin_immutable(self) -> bool {
        (self == std::ptr::addr_of_mut!(PyRange_Type))
            | (self == std::ptr::addr_of_mut!(PyFunction_Type))
            | (self == std::ptr::addr_of_mut!(PyCFunction_Type))
            | (self == std::ptr::addr_of_mut!(ffi_ext::PyProperty_Type))
            | (self == std::ptr::addr_of_mut!(ffi_ext::_PyWeakref_RefType))
            | (self == std::ptr::addr_of_mut!(PyCode_Type))
            | (self == std::ptr::addr_of_mut!(_PyNotImplemented_Type))
            | (self == std::ptr::addr_of_mut!(ffi_ext::PyEllipsis_Type))
            | (self == std::ptr::addr_of_mut!(PyComplex_Type))
    }

    #[inline(always)]
    unsafe fn is_stdlib_immutable(self) -> bool {
        let state_pointer = std::ptr::addr_of!(STATE);
        let regex_pattern_type = (*state_pointer).re_pattern_type;
        let decimal_type = (*state_pointer).decimal_type;
        let fraction_type = (*state_pointer).fraction_type;
        (self == regex_pattern_type) || (self == decimal_type) || (self == fraction_type)
    }

    #[inline(always)]
    unsafe fn is_type_subclass(self) -> bool {
        (crate::ffi_ext::tp_flags_of(self) & (Py_TPFLAGS_TYPE_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_atomic_immutable(self) -> bool {
        self.is_literal_immutable()
            || self.is_builtin_immutable()
            || self.is_type_subclass()
            || self.is_stdlib_immutable()
    }
}
