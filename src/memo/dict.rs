use pyo3_ffi::*;
use std::ptr;

use super::Memo;
use crate::py;
use crate::types::{py_list_new, PyMapPtr, PyMutSeqPtr, PyObjectPtr, PyTypeInfo};

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

            let pykey = self.dict.id();
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
            if !py::err::occurred().is_null() {
                pykey.decref();
                return -1;
            }

            let list = py_list_new(0);
            if list.is_null() {
                pykey.decref();
                return -1;
            }

            if self.dict.set_item(pykey, list) < 0 {
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

impl Memo for DictMemo {
    type Probe = ();
    const RECALL_CAN_ERROR: bool = true;

    #[inline(always)]
    unsafe fn recall<T: PyTypeInfo>(&mut self, object: *mut T) -> ((), *mut T) {
        unsafe {
            let pykey = object.id();
            if pykey.is_null() {
                return ((), ptr::null_mut());
            }

            let found = self.dict.get_item(pykey);
            if !found.is_null() {
                found.incref();
            }
            pykey.decref();
            ((), T::cast_unchecked(found))
        }
    }

    #[inline(always)]
    unsafe fn memoize<T: PyTypeInfo>(
        &mut self,
        original: *mut T,
        copy: *mut T,
        _probe: &(),
    ) -> i32 {
        unsafe {
            let pykey = original.id();
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

            self.keepalive.append(original)
        }
    }

    #[inline(always)]
    unsafe fn forget<T: PyTypeInfo>(&mut self, _original: *mut T, _probe: &()) {}

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
