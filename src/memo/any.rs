use std::ptr;

use super::Memo;
use crate::py::{self, *};
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

            let existing = py::call::method_obj_args!(
                self.object,
                py_str!("get"),
                pykey,
                sentinel,
            );
            if existing.is_null() {
                pykey.decref();
                return -1;
            }

            if existing != sentinel {
                self.keepalive = existing as *mut PyListObject;
                pykey.decref();
                return 0;
            }

            existing.decref();

            let list = py::list::new(0);
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

            let found = py::call::method_obj_args!(
                self.object,
                py_str!("get"),
                pykey,
                sentinel,
            );
            pykey.decref();

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
