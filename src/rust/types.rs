//! Type checking and identification

use crate::ffi::*;
use std::sync::OnceLock;
use std::ptr::addr_of_mut;

/// Cached type pointers for fast exact-type checks
pub struct TypeCache {
    pub dict: *mut PyTypeObject,
    pub list: *mut PyTypeObject,
    pub tuple: *mut PyTypeObject,
    pub set: *mut PyTypeObject,
    pub frozenset: *mut PyTypeObject,
    pub bytearray: *mut PyTypeObject,
    pub long: *mut PyTypeObject,
    pub float: *mut PyTypeObject,
    pub unicode: *mut PyTypeObject,
    pub bytes: *mut PyTypeObject,
    pub bool_: *mut PyTypeObject,
    pub complex: *mut PyTypeObject,
    pub range: *mut PyTypeObject,
}

// SAFETY: We're just holding pointers to global Python type objects
unsafe impl Send for TypeCache {}
unsafe impl Sync for TypeCache {}

static TYPE_CACHE: OnceLock<TypeCache> = OnceLock::new();

/// Initialize type cache
pub fn init_type_cache() {
    TYPE_CACHE.get_or_init(|| {
        TypeCache {
            dict: addr_of_mut!(PyDict_Type),
            list: addr_of_mut!(PyList_Type),
            tuple: addr_of_mut!(PyTuple_Type),
            set: addr_of_mut!(PySet_Type),
            frozenset: addr_of_mut!(PyFrozenSet_Type),
            bytearray: addr_of_mut!(PyByteArray_Type),
            long: addr_of_mut!(PyLong_Type),
            float: addr_of_mut!(PyFloat_Type),
            unicode: addr_of_mut!(PyUnicode_Type),
            bytes: addr_of_mut!(PyBytes_Type),
            bool_: addr_of_mut!(PyBool_Type),
            complex: addr_of_mut!(PyComplex_Type),
            range: addr_of_mut!(PyRange_Type),
        }
    });
}

/// Get type cache
#[inline(always)]
pub fn get_type_cache() -> &'static TypeCache {
    TYPE_CACHE.get().unwrap()
}

/// Type classification for dispatch
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TypeClass {
    ImmutableLiteral,
    Dict,
    List,
    Tuple,
    Set,
    FrozenSet,
    ByteArray,
    CustomDeepCopy,
    RequiresReduce,
}

/// Classify object type ONCE
#[inline]
pub unsafe fn classify_type(obj: *mut PyObject) -> TypeClass {
    let tp = Py_TYPE(obj);

    // Fast path: immutable literals
    if is_immutable_literal(obj) {
        return TypeClass::ImmutableLiteral;
    }

    let cache = get_type_cache();

    // Exact type checks (hot path)
    if tp == cache.dict {
        return TypeClass::Dict;
    }
    if tp == cache.list {
        return TypeClass::List;
    }
    if tp == cache.tuple {
        return TypeClass::Tuple;
    }
    if tp == cache.set {
        return TypeClass::Set;
    }
    if tp == cache.frozenset {
        return TypeClass::FrozenSet;
    }
    if tp == cache.bytearray {
        return TypeClass::ByteArray;
    }
    
    // Range is immutable
    if tp == cache.range {
        return TypeClass::ImmutableLiteral;
    }

    TypeClass::RequiresReduce
}

/// Check if type has __deepcopy__ method
pub unsafe fn has_deepcopy(obj: *mut PyObject) -> bool {
    let deepcopy_str = PyUnicode_InternFromString(b"__deepcopy__\0".as_ptr() as *const i8);
    if deepcopy_str.is_null() {
        return false;
    }

    let attr = PyObject_GetAttr(obj, deepcopy_str);
    Py_DECREF(deepcopy_str);

    if !attr.is_null() {
        Py_DECREF(attr);
        true
    } else {
        PyErr_Clear();
        false
    }
}
