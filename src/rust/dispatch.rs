//! Type dispatch for deepcopy - simplified

use crate::ffi::*;
use crate::types::{TypeClass, has_deepcopy};
use crate::state::ThreadLocalMemo;
use crate::containers;
use crate::reduce;

/// Dispatch to appropriate handler based on type
#[inline]
pub unsafe fn dispatch_deepcopy(
    obj: *mut PyObject,
    type_class: TypeClass,
    hash: Py_ssize_t,
    memo: &mut ThreadLocalMemo,
) -> Result<*mut PyObject, String> {
    match type_class {
        TypeClass::ImmutableLiteral => {
            // Fast path: immutable literals
            Ok(Py_NewRef(obj))
        }

        TypeClass::Dict => {
            containers::deepcopy_dict(obj, memo)
        }

        TypeClass::List => {
            containers::deepcopy_list(obj, memo)
        }

        TypeClass::Tuple => {
            containers::deepcopy_tuple(obj, hash, memo)
        }

        TypeClass::Set => {
            containers::deepcopy_set(obj, memo)
        }

        TypeClass::FrozenSet => {
            containers::deepcopy_frozenset(obj, memo)
        }

        TypeClass::ByteArray => {
            containers::deepcopy_bytearray(obj, memo)
        }

        TypeClass::CustomDeepCopy => {
            // Has __deepcopy__ method
            call_custom_deepcopy(obj, memo)
        }

        TypeClass::RequiresReduce => {
            // Check for __deepcopy__ first
            if has_deepcopy(obj) {
                call_custom_deepcopy(obj, memo)
            } else {
                reduce::deepcopy_via_reduce(obj, memo)
            }
        }
    }
}

/// Call custom __deepcopy__ method
unsafe fn call_custom_deepcopy(
    obj: *mut PyObject,
    memo: &mut ThreadLocalMemo,
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

    // Create proxy if needed - for now use empty dict (simplified)
    let memo_arg = PyDict_New();
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
        // Save result to memo
        let key = obj as *const std::os::raw::c_void;
        let hash = hash_pointer(key as *mut std::os::raw::c_void);
        memo.table.insert(key, result, hash);
        memo.keepalive.append(result);

        Ok(result)
    }
}

extern "C" {
    fn PyUnicode_InternFromString(s: *const i8) -> *mut PyObject;
}
