//! Type dispatch for deepcopy
//!
//! Handles routing to specialized handlers based on type classification.
//! Type is computed ONCE and used throughout.

use crate::ffi::*;
use crate::types::{TypeClass, classify_type, has_deepcopy};
use crate::state::MemoState;
use crate::containers;
use crate::reduce;
use std::ptr;

/// Dispatch to appropriate handler based on type
#[inline]
pub unsafe fn dispatch_deepcopy(
    obj: *mut PyObject,
    type_class: TypeClass,
    hash: Py_ssize_t,
    state: &mut MemoState,
) -> Result<*mut PyObject, String> {
    match type_class {
        TypeClass::ImmutableLiteral => {
            // Fast path: immutable literals
            Ok(Py_NewRef(obj))
        }

        TypeClass::Dict => {
            containers::deepcopy_dict(obj, state)
        }

        TypeClass::List => {
            containers::deepcopy_list(obj, state)
        }

        TypeClass::Tuple => {
            containers::deepcopy_tuple(obj, hash, state)
        }

        TypeClass::Set => {
            containers::deepcopy_set(obj, state)
        }

        TypeClass::FrozenSet => {
            containers::deepcopy_frozenset(obj, state)
        }

        TypeClass::ByteArray => {
            containers::deepcopy_bytearray(obj, state)
        }

        TypeClass::CustomDeepCopy => {
            // Has __deepcopy__ method
            call_custom_deepcopy(obj, state)
        }

        TypeClass::RequiresReduce => {
            // Check for __deepcopy__ first
            if has_deepcopy(obj) {
                call_custom_deepcopy(obj, state)
            } else {
                reduce::deepcopy_via_reduce(obj, state)
            }
        }
    }
}

/// Call custom __deepcopy__ method
unsafe fn call_custom_deepcopy(
    obj: *mut PyObject,
    state: &mut MemoState,
) -> Result<*mut PyObject, String> {
    // Get __deepcopy__ attribute
    let deepcopy_str = PyUnicode_InternFromString(b"__deepcopy__\0".as_ptr() as *const i8);
    if deepcopy_str.is_null() {
        return Err("Failed to create __deepcopy__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, deepcopy_str);
    Py_DecRef(deepcopy_str);

    if method.is_null() {
        return Err("Object has no __deepcopy__ method".to_string());
    }

    // Create proxy if needed
    // For now, simplified - would need to handle proxy creation
    let memo_arg = PyDict_New(); // Temporary: use empty dict
    if memo_arg.is_null() {
        Py_DecRef(method);
        return Err("Failed to create memo dict".to_string());
    }

    // Call __deepcopy__(memo)
    let result = PyObject_CallOneArg(method, memo_arg);
    Py_DecRef(method);
    Py_DecRef(memo_arg);

    if result.is_null() {
        Err("__deepcopy__ call failed".to_string())
    } else {
        Ok(result)
    }
}

extern "C" {
    fn PyUnicode_InternFromString(s: *const i8) -> *mut PyObject;
}
