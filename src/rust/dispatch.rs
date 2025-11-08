//! Type dispatch for deepcopy

use crate::ffi::*;
use crate::types::{TypeClass, has_deepcopy};
use crate::memo_trait::Memo;
use crate::containers;
use crate::reduce;

/// Dispatch to appropriate handler based on type - generic over Memo
#[inline]
pub unsafe fn dispatch_deepcopy<M: Memo>(
    obj: *mut PyObject,
    type_class: TypeClass,
    hash: Py_ssize_t,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    match type_class {
        TypeClass::ImmutableLiteral => {
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
            call_custom_deepcopy(obj, memo)
        }

        TypeClass::RequiresReduce => {
            if has_deepcopy(obj) {
                call_custom_deepcopy(obj, memo)
            } else {
                reduce::deepcopy_via_reduce(obj, memo)
            }
        }
    }
}

/// Call custom __deepcopy__ method - generic over Memo
unsafe fn call_custom_deepcopy<M: Memo>(
    obj: *mut PyObject,
    memo: &mut M,
) -> Result<*mut PyObject, String> {
    let deepcopy_str = PyUnicode_InternFromString(b"__deepcopy__\0".as_ptr() as *const i8);
    if deepcopy_str.is_null() {
        return Err("Failed to create __deepcopy__ string".to_string());
    }

    let method = PyObject_GetAttr(obj, deepcopy_str);
    Py_DECREF(deepcopy_str);

    if method.is_null() {
        return Err("Object has no __deepcopy__ method".to_string());
    }

    // Get the Python dict representation of the memo
    let memo_arg = memo.as_python_dict();

    if memo_arg.is_null() {
        Py_DECREF(method);
        return Err("Failed to create memo dict".to_string());
    }

    let result = crate::ffi::call_one_arg(method, memo_arg);
    Py_DECREF(method);
    Py_DECREF(memo_arg);

    if result.is_null() {
        PyErr_Clear();
        Err("__deepcopy__ call failed".to_string())
    } else {
        // Save result to memo
        let key = obj as *const std::os::raw::c_void;
        let hash = hash_pointer(key as *mut std::os::raw::c_void);
        memo.insert(key, result, hash);
        memo.keepalive(obj);  // Keep the original object alive, not the result

        Ok(result)
    }
}
