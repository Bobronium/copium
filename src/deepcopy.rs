//! Core deepcopy implementation
//! Following the optimized flow with compile-time state management

use crate::ffi::{self, PyObject, PyTypeObject};
use crate::proxy::{create_memo_proxy, get_thread_memo, reset_thread_memo};
use crate::reconstructor::*;
use crate::types::{CopyContext, CopyResult, ObjectType, Uninitialized};
use pyo3::prelude::*;
use std::ffi::CStr;
use std::os::raw::c_void;
use std::ptr;

/// Check if type is atomic immutable (no deepcopy needed)
#[inline(always)]
unsafe fn is_atomic_immutable(tp: *mut PyTypeObject) -> bool {
    // Tier 1: Most common literal immutables
    if is_literal_immutable(tp) {
        return true;
    }

    // Tier 2: Builtin immutables
    if is_builtin_immutable(tp) {
        return true;
    }

    // Tier 3: Type objects themselves
    if is_class(tp) {
        return true;
    }

    false
}

#[inline(always)]
unsafe fn is_literal_immutable(tp: *mut PyTypeObject) -> bool {
    // Check against known immutable types
    // None, int, str, bool, float, bytes
    let type_name = CStr::from_ptr((*tp).tp_name).to_bytes();

    matches!(
        type_name,
        b"NoneType" | b"int" | b"str" | b"bool" | b"float" | b"bytes"
    )
}

#[inline(always)]
unsafe fn is_builtin_immutable(tp: *mut PyTypeObject) -> bool {
    let type_name = CStr::from_ptr((*tp).tp_name).to_bytes();

    matches!(
        type_name,
        b"range"
            | b"function"
            | b"builtin_function_or_method"
            | b"method"
            | b"property"
            | b"weakref"
            | b"code"
            | b"module"
            | b"NotImplementedType"
            | b"ellipsis"
            | b"complex"
    )
}

#[inline(always)]
unsafe fn is_class(tp: *mut PyTypeObject) -> bool {
    const Py_TPFLAGS_TYPE_SUBCLASS: i64 = 1 << 31;
    ffi::PyType_HasFeature(tp, Py_TPFLAGS_TYPE_SUBCLASS) != 0
}

/// Main deepcopy entry point with optional memo
pub fn deepcopy_impl(py: Python, obj: &PyAny, memo: Option<&PyAny>) -> PyResult<PyObject> {
    let obj_ptr = obj.as_ptr();

    unsafe {
        let tp = ffi::py_type(obj_ptr);

        // Fast path: immutable objects
        if is_atomic_immutable(tp) {
            ffi::incref(obj_ptr);
            return Ok(PyObject::from_owned_ptr(py, obj_ptr));
        }

        // Initialize context
        let ctx = CopyContext::<Uninitialized, _>::new();

        let result = if let Some(user_memo) = memo {
            // User provided memo
            let ctx = ctx.with_user_memo();
            deepcopy_with_user_memo(py, obj_ptr, tp, user_memo.as_ptr(), ctx)
        } else {
            // Use thread-local memo
            deepcopy_with_thread_memo(py, obj_ptr, tp, ctx)
        };

        match result {
            CopyResult::Immutable(p) | CopyResult::Mutable(p) | CopyResult::FromMemo(p) => {
                Ok(PyObject::from_owned_ptr(py, p))
            }
            CopyResult::Error => Err(PyErr::fetch(py)),
        }
    }
}

/// Deepcopy with user-provided memo
unsafe fn deepcopy_with_user_memo(
    py: Python,
    obj: *mut PyObject,
    tp: *mut PyTypeObject,
    user_memo: *mut PyObject,
    ctx: CopyContext<crate::types::FromUser, crate::types::NoHash>,
) -> CopyResult {
    // Check if object is in user's memo
    let key = ffi::PyLong_FromVoidPtr(obj as *const c_void);
    if key.is_null() {
        return CopyResult::Error;
    }

    let found = ffi::PyDict_GetItem(user_memo, key);
    ffi::decref(key);

    if !found.is_null() {
        ffi::incref(found);
        return CopyResult::FromMemo(found);
    }

    // Not in memo, do the copy
    dispatch_copy(py, obj, tp, Some(user_memo), false)
}

/// Deepcopy with thread-local memo
unsafe fn deepcopy_with_thread_memo(
    py: Python,
    obj: *mut PyObject,
    tp: *mut PyTypeObject,
    ctx: CopyContext<Uninitialized, crate::types::NoHash>,
) -> CopyResult {
    dispatch_copy(py, obj, tp, None, true)
}

/// Dispatch to appropriate copy method
#[inline(always)]
unsafe fn dispatch_copy(
    py: Python,
    obj: *mut PyObject,
    tp: *mut PyTypeObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    let type_name = CStr::from_ptr((*tp).tp_name).to_bytes();

    // Try specialized reconstructors first
    match type_name {
        b"dict" => return copy_dict(py, obj, user_memo, use_thread_memo),
        b"list" => return copy_list(py, obj, user_memo, use_thread_memo),
        b"set" => return copy_set(py, obj, user_memo, use_thread_memo),
        b"frozenset" => return copy_frozenset(py, obj, user_memo, use_thread_memo),
        b"tuple" => return copy_tuple(py, obj, user_memo, use_thread_memo),
        _ => {}
    }

    // Check for __deepcopy__ method
    if has_deepcopy_method(obj) {
        return call_deepcopy_method(py, obj, user_memo, use_thread_memo);
    }

    // Fall back to reduce protocol
    copy_via_reduce(py, obj, user_memo, use_thread_memo)
}

/// Check if object has __deepcopy__ method
#[inline(always)]
unsafe fn has_deepcopy_method(obj: *mut PyObject) -> bool {
    let deepcopy_str = b"__deepcopy__\0".as_ptr() as *const i8;
    let method = ffi::PyObject_GetAttrString(obj, deepcopy_str);
    if method.is_null() {
        ffi::PyErr_Clear();
        return false;
    }
    ffi::decref(method);
    true
}

/// Call __deepcopy__(memo) method
unsafe fn call_deepcopy_method(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    let deepcopy_str = b"__deepcopy__\0".as_ptr() as *const i8;
    let method = ffi::PyObject_GetAttrString(obj, deepcopy_str);
    if method.is_null() {
        return CopyResult::Error;
    }

    // Get or create proxy
    let memo_arg = if let Some(user_memo) = user_memo {
        ffi::incref(user_memo);
        user_memo
    } else if use_thread_memo {
        match create_memo_proxy(py) {
            Ok(proxy) => proxy.as_ptr(),
            Err(_) => {
                ffi::decref(method);
                return CopyResult::Error;
            }
        }
    } else {
        py.None().as_ptr()
    };

    let result = ffi::PyObject_CallOneArg(method, memo_arg);
    ffi::decref(method);
    ffi::decref(memo_arg);

    if result.is_null() {
        CopyResult::Error
    } else {
        CopyResult::Mutable(result)
    }
}

/// Copy via reduce protocol
unsafe fn copy_via_reduce(
    py: Python,
    obj: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    // Try __reduce_ex__(4) first
    let reduce_ex_str = b"__reduce_ex__\0".as_ptr() as *const i8;
    let reduce_ex = ffi::PyObject_GetAttrString(obj, reduce_ex_str);

    let reduce_result = if !reduce_ex.is_null() {
        let protocol = ffi::PyLong_FromLongLong(4);
        let result = ffi::PyObject_CallOneArg(reduce_ex, protocol);
        ffi::decref(protocol);
        ffi::decref(reduce_ex);
        result
    } else {
        ffi::PyErr_Clear();
        let reduce_str = b"__reduce__\0".as_ptr() as *const i8;
        let reduce = ffi::PyObject_GetAttrString(obj, reduce_str);
        if reduce.is_null() {
            return CopyResult::Error;
        }
        let result = ffi::PyObject_Call(reduce, ptr::null_mut(), ptr::null_mut());
        ffi::decref(reduce);
        result
    };

    if reduce_result.is_null() {
        return CopyResult::Error;
    }

    // Reconstruct from reduce result
    // For now, simplified version
    let reconstructed = reconstruct_from_reduce(py, reduce_result, user_memo, use_thread_memo);
    ffi::decref(reduce_result);

    reconstructed
}

/// Reconstruct object from __reduce__ result
unsafe fn reconstruct_from_reduce(
    py: Python,
    reduce_result: *mut PyObject,
    user_memo: Option<*mut PyObject>,
    use_thread_memo: bool,
) -> CopyResult {
    // Simplified: call constructor with args
    // Full implementation would handle all reduce protocol cases

    if ffi::PyTuple_GET_ITEM.is_none() {
        return CopyResult::Error;
    }

    let constructor = ffi::PyTuple_GET_ITEM(reduce_result, 0);
    let args = ffi::PyTuple_GET_ITEM(reduce_result, 1);

    if constructor.is_null() || args.is_null() {
        return CopyResult::Error;
    }

    // Deep copy args
    let copied_args = match dispatch_copy(py, args, ffi::py_type(args), user_memo, use_thread_memo) {
        CopyResult::Mutable(p) | CopyResult::Immutable(p) => p,
        CopyResult::FromMemo(p) => p,
        CopyResult::Error => return CopyResult::Error,
    };

    let result = ffi::PyObject_Call(constructor, copied_args, ptr::null_mut());
    ffi::decref(copied_args);

    if result.is_null() {
        CopyResult::Error
    } else {
        CopyResult::Mutable(result)
    }
}

/// Cleanup after deepcopy
pub fn cleanup_after_call() {
    reset_thread_memo();
}
