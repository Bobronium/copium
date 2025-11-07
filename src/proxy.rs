//! Proxy types for exposing Memo and Keepalive to Python
//! - MemoProxy: implements dict protocol, wraps MemoTable
//! - KeepListProxy: implements list protocol, wraps KeepVector
//! - Created on-demand when __deepcopy__(memo) is called

use crate::ffi::{self, PyObject};
use crate::memo::ThreadMemo;
use pyo3::prelude::*;
use pyo3::types::{PyDict, PyList};
use std::cell::RefCell;
use std::ptr;

thread_local! {
    static THREAD_MEMO: RefCell<Option<Box<ThreadMemo>>> = RefCell::new(None);
}

/// Get or create thread-local memo
pub fn get_thread_memo() -> &'static mut ThreadMemo {
    THREAD_MEMO.with(|tm| {
        let mut tm_ref = tm.borrow_mut();
        if tm_ref.is_none() {
            *tm_ref = Some(Box::new(ThreadMemo::new()));
        }
        // SAFETY: We're returning a mutable reference to thread-local data
        // This is safe because it's thread-local and we control access
        unsafe { std::mem::transmute(tm_ref.as_mut().unwrap().as_mut()) }
    })
}

/// Reset thread-local memo (called after deepcopy)
pub fn reset_thread_memo() {
    THREAD_MEMO.with(|tm| {
        if let Some(ref mut memo) = *tm.borrow_mut() {
            memo.reset();
        }
    });
}

/// Proxy for Memo - implements dict protocol
#[pyclass(name = "_Memo")]
pub struct MemoProxy {
    // Reference to thread-local memo (no ownership)
    _phantom: std::marker::PhantomData<()>,
}

#[pymethods]
impl MemoProxy {
    #[new]
    fn new() -> Self {
        Self {
            _phantom: std::marker::PhantomData,
        }
    }

    fn __len__(&self) -> usize {
        let memo = get_thread_memo();
        memo.table.iter_info().0
    }

    fn __getitem__(&self, py: Python, key: &PyAny) -> PyResult<PyObject> {
        let key_int: usize = key.extract()?;
        let key_ptr = key_int as *const std::os::raw::c_void;

        // Special case: memo[id(memo)] returns keepalive proxy
        let memo = get_thread_memo();
        let memo_ptr = memo as *const _ as *const std::os::raw::c_void;

        if key_ptr == memo_ptr {
            return Ok(KeepListProxy::new().into_py(py));
        }

        let hash = ffi::hash_pointer(key_ptr);
        let value = memo.table.lookup_with_hash(key_ptr, hash);

        if value.is_null() {
            Err(pyo3::exceptions::PyKeyError::new_err("key not found"))
        } else {
            unsafe { Ok(PyObject::from_borrowed_ptr(py, value)) }
        }
    }

    fn __setitem__(&self, py: Python, key: &PyAny, value: &PyAny) -> PyResult<()> {
        let key_int: usize = key.extract()?;
        let key_ptr = key_int as *const std::os::raw::c_void;
        let value_ptr = value.as_ptr();

        let memo = get_thread_memo();
        let hash = ffi::hash_pointer(key_ptr);

        memo.table
            .insert_with_hash(key_ptr, value_ptr, hash)
            .map_err(|_| pyo3::exceptions::PyMemoryError::new_err("failed to insert"))?;

        Ok(())
    }

    fn __contains__(&self, key: &PyAny) -> PyResult<bool> {
        let key_int: usize = key.extract()?;
        let key_ptr = key_int as *const std::os::raw::c_void;

        let memo = get_thread_memo();
        let hash = ffi::hash_pointer(key_ptr);
        let value = memo.table.lookup_with_hash(key_ptr, hash);

        Ok(!value.is_null())
    }

    fn get(&self, py: Python, key: &PyAny, default: Option<&PyAny>) -> PyResult<PyObject> {
        let key_int: usize = key.extract()?;
        let key_ptr = key_int as *const std::os::raw::c_void;

        let memo = get_thread_memo();
        let hash = ffi::hash_pointer(key_ptr);
        let value = memo.table.lookup_with_hash(key_ptr, hash);

        if value.is_null() {
            Ok(default.map(|d| d.into()).unwrap_or_else(|| py.None()))
        } else {
            unsafe { Ok(PyObject::from_borrowed_ptr(py, value)) }
        }
    }

    fn setdefault(&self, py: Python, key: &PyAny, default: Option<&PyAny>) -> PyResult<PyObject> {
        let key_int: usize = key.extract()?;
        let key_ptr = key_int as *const std::os::raw::c_void;

        let memo = get_thread_memo();

        // Special case: id(memo) returns keepalive proxy
        let memo_ptr = memo as *const _ as *const std::os::raw::c_void;
        if key_ptr == memo_ptr {
            return Ok(KeepListProxy::new().into_py(py));
        }

        let hash = ffi::hash_pointer(key_ptr);
        let value = memo.table.lookup_with_hash(key_ptr, hash);

        if value.is_null() {
            let default_obj = default.map(|d| d.as_ptr()).unwrap_or_else(|| py.None().as_ptr());
            memo.table
                .insert_with_hash(key_ptr, default_obj, hash)
                .map_err(|_| pyo3::exceptions::PyMemoryError::new_err("failed to insert"))?;
            unsafe { Ok(PyObject::from_borrowed_ptr(py, default_obj)) }
        } else {
            unsafe { Ok(PyObject::from_borrowed_ptr(py, value)) }
        }
    }

    fn clear(&self) {
        let memo = get_thread_memo();
        memo.table.clear();
    }

    fn keep(&self, py: Python) -> PyObject {
        KeepListProxy::new().into_py(py)
    }
}

/// Proxy for keepalive - implements list protocol
#[pyclass(name = "_KeepList")]
pub struct KeepListProxy {
    _phantom: std::marker::PhantomData<()>,
}

#[pymethods]
impl KeepListProxy {
    #[new]
    fn new() -> Self {
        Self {
            _phantom: std::marker::PhantomData,
        }
    }

    fn __len__(&self) -> usize {
        let memo = get_thread_memo();
        memo.keepalive.len()
    }

    fn __getitem__(&self, py: Python, index: isize) -> PyResult<PyObject> {
        let memo = get_thread_memo();
        let len = memo.keepalive.len() as isize;

        let idx = if index < 0 {
            len + index
        } else {
            index
        };

        if idx < 0 || idx >= len {
            return Err(pyo3::exceptions::PyIndexError::new_err("index out of range"));
        }

        let item = memo.keepalive.get(idx as usize)
            .ok_or_else(|| pyo3::exceptions::PyIndexError::new_err("index out of range"))?;

        unsafe { Ok(PyObject::from_borrowed_ptr(py, item)) }
    }

    fn append(&self, item: &PyAny) -> PyResult<()> {
        let memo = get_thread_memo();
        memo.keepalive
            .append(item.as_ptr())
            .map_err(|_| pyo3::exceptions::PyMemoryError::new_err("failed to append"))?;
        Ok(())
    }

    fn clear(&self) {
        let memo = get_thread_memo();
        memo.keepalive.clear();
    }
}

/// Create memo proxy for __deepcopy__ call
pub fn create_memo_proxy(py: Python) -> PyResult<PyObject> {
    Ok(MemoProxy::new().into_py(py))
}
