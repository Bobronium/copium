use pyo3_ffi::*;
use std::ffi::CStr;
use std::os::raw::c_int;

use super::{ffi, PyTypeInfo};

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
pub type PyDictWatchCallback = Option<
    unsafe extern "C" fn(
        event: i32,
        dict: *mut PyObject,
        key: *mut PyObject,
        new_value: *mut PyObject,
    ) -> i32,
>;

pub unsafe trait PyMapPtr: Sized {
    unsafe fn size(self) -> Py_ssize_t;
    #[inline(always)]
    unsafe fn len(self) -> Py_ssize_t {
        self.size()
    }

    unsafe fn set_item<K: PyTypeInfo, V: PyTypeInfo>(self, key: *mut K, value: *mut V) -> c_int;
    unsafe fn steal_item<K: PyTypeInfo, V: PyTypeInfo>(self, key: *mut K, value: *mut V)
        -> c_int;
    unsafe fn get_item<K: PyTypeInfo>(self, key: *mut K) -> *mut PyObject;
    unsafe fn borrow_item_cstr(self, key: &CStr) -> *mut PyObject;
    unsafe fn dict_copy(self) -> *mut PyDictObject;
    unsafe fn merge<T: PyTypeInfo>(self, other: *mut T, override_existing: bool) -> c_int;
    unsafe fn dict_next(
        self,
        position: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        value: &mut *mut PyObject,
    ) -> bool;
    unsafe fn set_item_cstr<V: PyTypeInfo>(self, key: &CStr, value: *mut V) -> c_int;
    unsafe fn watch(self, watcher_id: i32) -> c_int;
    unsafe fn unwatch(self, watcher_id: i32) -> c_int;
}

unsafe impl PyMapPtr for *mut PyDictObject {
    #[inline(always)]
    unsafe fn size(self) -> Py_ssize_t {
        pyo3_ffi::PyDict_Size(self as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn set_item<K: PyTypeInfo, V: PyTypeInfo>(self, key: *mut K, value: *mut V) -> c_int {
        pyo3_ffi::PyDict_SetItem(
            self as *mut PyObject,
            key as *mut PyObject,
            value as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn steal_item<K: PyTypeInfo, V: PyTypeInfo>(
        self,
        key: *mut K,
        value: *mut V,
    ) -> c_int {
        ffi::_PyDict_SetItem_Take2(
            self as *mut PyObject,
            key as *mut PyObject,
            value as *mut PyObject,
        )
    }

    #[inline(always)]
    unsafe fn get_item<K: PyTypeInfo>(self, key: *mut K) -> *mut PyObject {
        pyo3_ffi::PyDict_GetItemWithError(self as *mut PyObject, key as *mut PyObject)
    }

    #[inline(always)]
    unsafe fn borrow_item_cstr(self, key: &CStr) -> *mut PyObject {
        pyo3_ffi::PyDict_GetItemString(self as *mut PyObject, key.as_ptr())
    }

    #[inline(always)]
    unsafe fn dict_copy(self) -> *mut PyDictObject {
        pyo3_ffi::PyDict_Copy(self as *mut PyObject) as *mut PyDictObject
    }

    #[inline(always)]
    unsafe fn merge<T: PyTypeInfo>(self, other: *mut T, override_existing: bool) -> c_int {
        pyo3_ffi::PyDict_Merge(
            self as *mut PyObject,
            other as *mut PyObject,
            override_existing as c_int,
        )
    }

    #[inline(always)]
    unsafe fn dict_next(
        self,
        position: &mut Py_ssize_t,
        key: &mut *mut PyObject,
        value: &mut *mut PyObject,
    ) -> bool {
        pyo3_ffi::PyDict_Next(self as *mut PyObject, position, key, value) != 0
    }

    #[inline(always)]
    unsafe fn set_item_cstr<V: PyTypeInfo>(self, key: &CStr, value: *mut V) -> c_int {
        pyo3_ffi::PyDict_SetItemString(
            self as *mut PyObject,
            key.as_ptr(),
            value as *mut PyObject,
        )
    }

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    #[inline(always)]
    unsafe fn watch(self, watcher_id: i32) -> c_int {
        ffi::PyDict_Watch(watcher_id, self as *mut PyObject)
    }

    #[cfg(not(all(Py_3_14, not(Py_GIL_DISABLED))))]
    #[inline(always)]
    unsafe fn watch(self, watcher_id: i32) -> c_int {
        let _ = self;
        let _ = watcher_id;
        -1
    }

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    #[inline(always)]
    unsafe fn unwatch(self, watcher_id: i32) -> c_int {
        ffi::PyDict_Unwatch(watcher_id, self as *mut PyObject)
    }

    #[cfg(not(all(Py_3_14, not(Py_GIL_DISABLED))))]
    #[inline(always)]
    unsafe fn unwatch(self, watcher_id: i32) -> c_int {
        let _ = self;
        let _ = watcher_id;
        -1
    }
}

#[inline(always)]
pub unsafe fn new() -> *mut PyDictObject {
    pyo3_ffi::PyDict_New() as *mut PyDictObject
}

#[inline(always)]
pub unsafe fn new_presized(length: Py_ssize_t) -> *mut PyDictObject {
    ffi::_PyDict_NewPresized(length) as *mut PyDictObject
}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
#[inline(always)]
pub unsafe fn add_watcher(callback: PyDictWatchCallback) -> i32 {
    ffi::PyDict_AddWatcher(callback)
}

#[cfg(not(all(Py_3_14, not(Py_GIL_DISABLED))))]
#[inline(always)]
pub unsafe fn add_watcher(
    callback: Option<
        unsafe extern "C" fn(
            event: i32,
            dict: *mut PyObject,
            key: *mut PyObject,
            new_value: *mut PyObject,
        ) -> i32,
    >,
) -> i32 {
    let _ = callback;
    -1
}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
#[inline(always)]
pub unsafe fn clear_watcher(watcher_id: i32) {
    let _ = ffi::PyDict_ClearWatcher(watcher_id);
}

#[cfg(not(all(Py_3_14, not(Py_GIL_DISABLED))))]
#[inline(always)]
pub unsafe fn clear_watcher(watcher_id: i32) {
    let _ = watcher_id;
}
