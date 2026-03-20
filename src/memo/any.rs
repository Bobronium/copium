use pyo3_ffi::*;
use std::ptr;

use super::Memo;
use crate::types::{py_list_new, py_tuple_new, PyMutSeqPtr, PyObjectPtr, PySeqPtr, PyTypeInfo};
use crate::{py_cache, py_eval, py_str};

pub struct AnyMemo {
    pub object: *mut PyObject,
    keepalive: *mut PyListObject,
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
            let pykey = self.object.id();
            if pykey.is_null() {
                return -1;
            }

            let getter = self.object.getattr(py_str!("get"));
            if getter.is_null() {
                pykey.decref();
                return -1;
            }
            let args = py_tuple_new(2);
            if args.is_null() {
                getter.decref();
                pykey.decref();
                return -1;
            }
            args.steal_item_unchecked(0, pykey.as_object());
            args.steal_item_unchecked(1, sentinel.newref());
            let existing = getter.call_with(args);
            getter.decref();
            args.decref();
            if existing.is_null() {
                return -1;
            }

            if existing != sentinel {
                self.keepalive = existing as *mut PyListObject;
                return 0;
            }

            existing.decref();

            let list = py_list_new(0);
            if list.is_null() {
                pykey.decref();
                return -1;
            }

            if self.object.setitem(pykey, list.as_object()) < 0 {
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

    unsafe fn recall<T: PyTypeInfo>(&mut self, object: *mut T) -> ((), *mut T) {
        unsafe {
            let sentinel = py_cache!(py_eval!("object()"));
            let pykey = object.id();
            if pykey.is_null() {
                return ((), ptr::null_mut());
            }

            let getter = self.object.getattr(py_str!("get"));
            if getter.is_null() {
                pykey.decref();
                return ((), ptr::null_mut());
            }
            let args = py_tuple_new(2);
            if args.is_null() {
                getter.decref();
                pykey.decref();
                return ((), ptr::null_mut());
            }
            args.steal_item_unchecked(0, pykey.as_object());
            args.steal_item_unchecked(1, sentinel.newref());
            let found = getter.call_with(args);
            getter.decref();
            args.decref();

            if found.is_null() {
                return ((), ptr::null_mut());
            }
            if found == sentinel {
                found.decref();
                return ((), ptr::null_mut());
            }

            ((), T::cast_unchecked(found))
        }
    }

    unsafe fn memoize<T: PyTypeInfo>(
        &mut self,
        original: *mut T,
        copy: *mut T,
        _probe: &Self::Probe,
    ) -> i32 {
        unsafe {
            let pykey = original.id();
            if pykey.is_null() {
                return -1;
            }

            let rc = self.object.setitem(pykey, copy.as_object());
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

    unsafe fn forget<T: PyTypeInfo>(&mut self, _original: *mut T, _probe: &Self::Probe) {}

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
