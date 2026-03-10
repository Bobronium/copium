use pyo3_ffi::*;
use std::ptr;

use crate::memo::{MemoCheckpoint, PyMemoObject};
use crate::state::{OnIncompatible, STATE};

/// After a __deepcopy__(native_memo) call fails with TypeError or AssertionError,
/// attempt retry with a plain dict memo.
///
/// Returns new reference to copied object, or NULL (with error restored/set).
///
/// TODO(DEVELOPMENT.md): full warning emission with traceback, call-site extraction,
/// and suppress_warnings matching (see _fallback.c _emit_fallback_warning).
pub unsafe fn maybe_retry_with_dict_memo(
    _obj: *mut PyObject,
    dunder_deepcopy: *mut PyObject,
    memo: &mut PyMemoObject,
    checkpoint: MemoCheckpoint,
) -> *mut PyObject {
    unsafe {
        if PyErr_ExceptionMatches(PyExc_TypeError) == 0
            && PyErr_ExceptionMatches(PyExc_AssertionError) == 0
        {
            return ptr::null_mut();
        }

        let s = &STATE;
        if s.on_incompatible == OnIncompatible::Raise {
            return ptr::null_mut();
        }

        // Save and clear the error
        let mut exc_type: *mut PyObject = ptr::null_mut();
        let mut exc_value: *mut PyObject = ptr::null_mut();
        let mut exc_tb: *mut PyObject = ptr::null_mut();
        PyErr_Fetch(&mut exc_type, &mut exc_value, &mut exc_tb);

        memo.rollback(checkpoint);

        let dict_memo = memo.to_dict();
        if dict_memo.is_null() {
            PyErr_Restore(exc_type, exc_value, exc_tb);
            return ptr::null_mut();
        }

        let dict_size_before = PyDict_Size(dict_memo);
        let result = PyObject_CallOneArg(dunder_deepcopy, dict_memo);

        if result.is_null() {
            Py_DECREF(dict_memo);
            Py_XDECREF(exc_type);
            Py_XDECREF(exc_value);
            Py_XDECREF(exc_tb);
            return ptr::null_mut();
        }

        if memo.sync_from_dict(dict_memo, dict_size_before) < 0 {
            Py_DECREF(result);
            Py_DECREF(dict_memo);
            Py_XDECREF(exc_type);
            Py_XDECREF(exc_value);
            Py_XDECREF(exc_tb);
            return ptr::null_mut();
        }

        // TODO: emit UserWarning when on_incompatible == Warn and error not in ignored_errors

        Py_DECREF(dict_memo);
        Py_XDECREF(exc_type);
        Py_XDECREF(exc_value);
        Py_XDECREF(exc_tb);
        PyErr_Clear();
        result
    }
}
