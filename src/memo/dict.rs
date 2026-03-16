use pyo3_ffi::*;
use std::ffi::c_void;
use std::ptr;

use super::Memo;
use crate::types::{py_list_new, PyMapPtr, PyObjectPtr};

pub struct DictMemo {
    pub dict: *mut PyDictObject,
    keepalive: *mut PyListObject,
}

impl DictMemo {
    pub fn new(dict: *mut PyDictObject) -> Self {
        Self {
            dict,
            keepalive: ptr::null_mut(),
        }
    }

    #[inline(never)]
    unsafe fn ensure_keepalive(&mut self) -> i32 {
        unsafe {
            if !self.keepalive.is_null() {
                return 0;
            }

            let pykey = PyLong_FromVoidPtr(self.dict as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let existing = self.dict.get_item(pykey);
            if !existing.is_null() {
                existing.incref();
                self.keepalive = existing as *mut PyListObject;
                pykey.decref();
                return 0;
            }
            if !PyErr_Occurred().is_null() {
                pykey.decref();
                return -1;
            }

            let list = py_list_new(0);
            if list.is_null() {
                pykey.decref();
                return -1;
            }

            if self.dict.set_item(pykey, list as *mut PyObject) < 0 {
                list.decref();
                pykey.decref();
                return -1;
            }

            self.keepalive = list as *mut PyListObject;
            pykey.decref();
            0
        }
    }
}

impl Memo for DictMemo {
    type Probe = ();
    const RECALL_CAN_ERROR: bool = true;

    #[inline(always)]
    unsafe fn recall(&mut self, object: *mut PyObject) -> ((), *mut PyObject) {
        unsafe {
            let pykey = PyLong_FromVoidPtr(object as *mut c_void);
            if pykey.is_null() {
                return ((), ptr::null_mut());
            }

            let found = self.dict.get_item(pykey);
            if !found.is_null() {
                found.incref();
            }
            pykey.decref();
            ((), found)
        }
    }

    #[inline(always)]
    unsafe fn memoize(&mut self, original: *mut PyObject, copy: *mut PyObject, _probe: &()) -> i32 {
        unsafe {
            let pykey = PyLong_FromVoidPtr(original as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let rc = self.dict.set_item(pykey, copy);
            pykey.decref();
            if rc < 0 {
                return -1;
            }

            if self.ensure_keepalive() < 0 {
                return -1;
            }

            PyList_Append(self.keepalive as *mut PyObject, original)
        }
    }

    #[inline(always)]
    unsafe fn forget(&mut self, _original: *mut PyObject, _probe: &()) {}

    #[inline(always)]
    unsafe fn as_call_arg(&mut self) -> *mut PyObject {
        self.dict as *mut PyObject
    }

    unsafe fn ensure_memo_is_valid(&mut self) -> i32 {
        unsafe { self.ensure_keepalive() }
    }
}

impl Drop for DictMemo {
    fn drop(&mut self) {
        if !self.keepalive.is_null() {
            unsafe { self.keepalive.decref() };
        }
    }
}
