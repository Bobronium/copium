use pyo3_ffi::*;
use std::ffi::c_void;
use std::ptr;

use super::Memo;
use crate::types::PyObjectPtr;
use crate::{py_cache, py_eval, py_str};

pub struct AnyMemo {
    pub object: *mut PyObject,
    keepalive: *mut PyObject,
}

impl AnyMemo {
    pub fn new(object: *mut PyObject) -> Self {
        Self {
            object,
            keepalive: ptr::null_mut(),
        }
    }

    unsafe fn ensure_keepalive(&mut self) -> i32 {
        unsafe {
            if !self.keepalive.is_null() {
                return 0;
            }

            let sentinel = py_cache!(py_eval!("object()"));
            let pykey = PyLong_FromVoidPtr(self.object as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let existing = PyObject_CallMethodObjArgs(
                self.object,
                py_str!("get"),
                pykey,
                sentinel,
                ptr::null_mut::<PyObject>(),
            );
            if existing.is_null() {
                pykey.decref();
                return -1;
            }

            if existing != sentinel {
                self.keepalive = existing;
                pykey.decref();
                return 0;
            }

            existing.decref();

            let list = PyList_New(0);
            if list.is_null() {
                pykey.decref();
                return -1;
            }

            if PyObject_SetItem(self.object, pykey, list) < 0 {
                list.decref();
                pykey.decref();
                return -1;
            }

            self.keepalive = list;
            pykey.decref();
            0
        }
    }
}

impl Memo for AnyMemo {
    type Probe = ();
    const RECALL_CAN_ERROR: bool = true;

    unsafe fn recall(&mut self, object: *mut PyObject) -> ((), *mut PyObject) {
        unsafe {
            let sentinel = py_cache!(py_eval!("object()"));
            let pykey = PyLong_FromVoidPtr(object as *mut c_void);
            if pykey.is_null() {
                return ((), ptr::null_mut());
            }

            let found = PyObject_CallMethodObjArgs(
                self.object,
                py_str!("get"),
                pykey,
                sentinel,
                ptr::null_mut::<PyObject>(),
            );
            pykey.decref();

            if found.is_null() {
                return ((), ptr::null_mut());
            }
            if found == sentinel {
                found.decref();
                return ((), ptr::null_mut());
            }

            ((), found)
        }
    }

    unsafe fn memoize(&mut self, original: *mut PyObject, copy: *mut PyObject, _probe: &()) -> i32 {
        unsafe {
            let pykey = PyLong_FromVoidPtr(original as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let rc = PyObject_SetItem(self.object, pykey, copy);
            pykey.decref();
            if rc < 0 {
                return -1;
            }

            if self.ensure_keepalive() < 0 {
                return -1;
            }

            PyList_Append(self.keepalive, original)
        }
    }

    unsafe fn forget(&mut self, _original: *mut PyObject, _probe: &()) {}

    #[inline(always)]
    unsafe fn as_call_arg(&mut self) -> *mut PyObject {
        self.object
    }

    unsafe fn ensure_memo_is_valid(&mut self) -> i32 {
        unsafe { self.ensure_keepalive() }
    }
}

impl Drop for AnyMemo {
    fn drop(&mut self) {
        if !self.keepalive.is_null() {
            unsafe { self.keepalive.decref() };
        }
    }
}
