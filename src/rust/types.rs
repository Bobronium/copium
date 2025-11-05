//! Type checking and identification
//!
//! This module provides efficient type checking using cached type pointers.
//! Type is computed ONCE and piped through the dispatch chain.

use crate::ffi::*;
use std::sync::OnceLock;

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
    pub none: *mut PyTypeObject,
}

// SAFETY: We're just holding pointers to global Python type objects
unsafe impl Send for TypeCache {}
unsafe impl Sync for TypeCache {}

static TYPE_CACHE: OnceLock<TypeCache> = OnceLock::new();

/// Initialize type cache
pub fn init_type_cache() {
    TYPE_CACHE.get_or_init(|| unsafe {
        TypeCache {
            dict: &mut PyDict_Type,
            list: &mut PyList_Type,
            tuple: &mut PyTuple_Type,
            set: &mut PySet_Type,
            frozenset: &mut PyFrozenSet_Type,
            bytearray: &mut PyByteArray_Type,
            long: &mut PyLong_Type,
            float: &mut PyFloat_Type,
            unicode: &mut PyUnicode_Type,
            bytes: &mut PyBytes_Type,
            bool_: &mut PyBool_Type,
            none: &mut _PyNone_Type,
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
    /// Immutable literals (None, int, str, bytes, bool, float)
    ImmutableLiteral,
    /// Dict (exact type)
    Dict,
    /// List (exact type)
    List,
    /// Tuple (exact type)
    Tuple,
    /// Set (exact type)
    Set,
    /// FrozenSet (exact type)
    FrozenSet,
    /// ByteArray
    ByteArray,
    /// Has __deepcopy__ method
    CustomDeepCopy,
    /// Requires reduce protocol
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

    // Check for __deepcopy__ (would need attribute lookup)
    // For now, fall back to reduce
    TypeClass::RequiresReduce
}

/// Check if type has __deepcopy__ method
pub unsafe fn has_deepcopy(obj: *mut PyObject) -> bool {
    // Create __deepcopy__ string
    let deepcopy_str = PyUnicode_InternFromString(b"__deepcopy__\0".as_ptr() as *const i8);
    if deepcopy_str.is_null() {
        return false;
    }

    let attr = PyObject_GetAttr(obj, deepcopy_str);
    Py_DecRef(deepcopy_str);

    if !attr.is_null() {
        Py_DecRef(attr);
        true
    } else {
        PyErr_Clear();
        false
    }
}

// Additional FFI function needed
extern "C" {
    pub fn PyUnicode_InternFromString(s: *const i8) -> *mut PyObject;
}
