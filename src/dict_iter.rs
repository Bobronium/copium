use pyo3_ffi::*;
use std::hint::{likely, unlikely};
use std::ptr;
#[cfg(Py_GIL_DISABLED)]
use crate::{py_cache_typed, py_obj};
use crate::types::PyObjectPtr;

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
extern "C" {
    fn PyDict_AddWatcher(
        callback: Option<
            unsafe extern "C" fn(
                event: i32,
                dict: *mut PyObject,
                key: *mut PyObject,
                new_value: *mut PyObject,
            ) -> i32,
        >,
    ) -> i32;
    fn PyDict_ClearWatcher(watcher_id: i32) -> i32;
    fn PyDict_Watch(watcher_id: i32, dict: *mut PyObject) -> i32;
    fn PyDict_Unwatch(watcher_id: i32, dict: *mut PyObject) -> i32;
}

#[cfg(all(Py_3_14, Py_GIL_DISABLED))]
extern "C" {
    fn PyIter_NextItem(iter: *mut PyObject, item: *mut *mut PyObject) -> i32;
}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
static mut G_DICT_WATCHER_ID: i32 = -1;

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
static mut G_DICT_WATCHER_REGISTERED: bool = false;

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
static mut G_GUARD_LIST_HEAD: *mut DictIterGuard = ptr::null_mut();

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
unsafe fn dict_watch_count(dict: *mut PyObject) -> i32 {
    let mut count = 0;
    let mut cur = unsafe { G_GUARD_LIST_HEAD };
    while !cur.is_null() {
        if likely(unsafe { (*cur).dict == dict }) {
            count += 1;
        }
        cur = unsafe { (*cur).next };
    }
    count
}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
unsafe extern "C" fn copium_dict_watcher_cb(
    _event: i32,
    dict: *mut PyObject,
    _key: *mut PyObject,
    _new_value: *mut PyObject,
) -> i32 {
    let mut cur_guard = unsafe { G_GUARD_LIST_HEAD };
    while !cur_guard.is_null() {
        let guard = unsafe { &mut *cur_guard };
        if likely(guard.dict == dict) {
            let cur = unsafe { PyDict_Size(dict) };
            if unlikely(cur >= 0 && cur != guard.size0) {
                guard.size_changed = true;
            }
            guard.mutated = true;
        }
        cur_guard = guard.next;
    }
    0
}

#[cfg(not(Py_3_14))]
#[repr(C)]
struct PyDictObjectCompat {
    ob_base: PyObject,
    ma_used: Py_ssize_t,
    ma_version_tag: u64,
}

#[cfg(not(Py_3_14))]
#[inline(always)]
unsafe fn dict_version_tag(dict: *mut PyObject) -> u64 {
    unsafe { (*(dict as *mut PyDictObjectCompat)).ma_version_tag }
}

#[cfg(not(Py_3_14))]
#[inline(always)]
unsafe fn dict_used(dict: *mut PyObject) -> Py_ssize_t {
    unsafe { (*(dict as *mut PyDictObjectCompat)).ma_used }
}

pub struct DictIterGuard {
    #[cfg(any(not(Py_3_14), Py_3_14))]
    dict: *mut PyObject,

    #[cfg(any(not(Py_3_14), all(Py_3_14, not(Py_GIL_DISABLED))))]
    pos: Py_ssize_t,

    #[cfg(not(Py_3_14))]
    ver0: u64,

    #[cfg(not(Py_3_14))]
    used0: Py_ssize_t,

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    size0: Py_ssize_t,

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    mutated: bool,

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    size_changed: bool,

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    prev: *mut DictIterGuard,

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    next: *mut DictIterGuard,

    #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
    it: *mut PyObject,

    #[cfg(Py_3_14)]
    active: bool,
}

impl DictIterGuard {
    #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
    #[inline(always)]
    fn cached_dict_items_vc() -> pyo3_ffi::vectorcallfunc {
        py_cache_typed!(pyo3_ffi::vectorcallfunc, {
            match crate::ffi_ext::PyVectorcall_Function(py_obj!("dict.items")) {
                Some(f) => Some(f),
                None => {
                    pyo3_ffi::PyErr_SetString(
                        pyo3_ffi::PyExc_TypeError,
                        crate::cstr!("copium: dict.items has no vectorcall"),
                    );
                    None
                }
            }
        })
    }

    #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
    unsafe fn new_dict_items_iterator(dict: *mut PyObject) -> *mut PyObject {
        unsafe {
            let arguments = [dict];
            let dict_items_view = (Self::cached_dict_items_vc())(
                py_obj!("dict.items"),
                arguments.as_ptr(),
                1,
                ptr::null_mut(),
            );
            if unlikely(dict_items_view.is_null()) {
                return ptr::null_mut();
            }

            let iterator = PyObject_GetIter(dict_items_view);
            dict_items_view.decref();
            iterator
        }
    }

    #[inline(always)]
    pub unsafe fn new(dict: *mut PyObject) -> Self {
        #[cfg(not(Py_3_14))]
        unsafe {
            Self {
                dict,
                pos: 0,
                ver0: dict_version_tag(dict),
                used0: dict_used(dict),
            }
        }

        #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
        unsafe {
            Self {
                dict,
                pos: 0,
                size0: PyDict_Size(dict),
                mutated: false,
                size_changed: false,
                prev: ptr::null_mut(),
                next: ptr::null_mut(),
                active: false,
            }
        }

        #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
        unsafe {
            Self {
                dict,
                it: Self::new_dict_items_iterator(dict),
                active: true,
            }
        }
    }

    #[inline(always)]
    pub unsafe fn activate(&mut self) {
        #[cfg(not(Py_3_14))]
        {
            let _ = self;
        }

        #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
        unsafe {
            if likely(!self.active) {
                self.active = true;
                self.register_watch();
            }
        }

        #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
        {
            let _ = self;
        }
    }

    #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
    unsafe fn register_watch(&mut self) {
        let self_ptr = self as *mut Self;

        let need_watch = unsafe { dict_watch_count(self.dict) == 0 };

        self.next = unsafe { G_GUARD_LIST_HEAD };
        if unlikely(!unsafe { G_GUARD_LIST_HEAD }.is_null()) {
            unsafe {
                (*G_GUARD_LIST_HEAD).prev = self_ptr;
            }
        }
        unsafe {
            G_GUARD_LIST_HEAD = self_ptr;
        }

        if unlikely(need_watch && unsafe { G_DICT_WATCHER_REGISTERED }) {
            let _ = unsafe { PyDict_Watch(G_DICT_WATCHER_ID, self.dict) };
        }
    }

    #[inline(always)]
    pub unsafe fn cleanup(&mut self) {
        #[cfg(not(Py_3_14))]
        {
            let _ = self;
        }

        #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
        unsafe {
            if unlikely(!self.active) {
                return;
            }

            self.active = false;
            let dict = self.dict;

            if likely(!self.prev.is_null()) {
                (*self.prev).next = self.next;
            } else {
                G_GUARD_LIST_HEAD = self.next;
            }
            if likely(!self.next.is_null()) {
                (*self.next).prev = self.prev;
            }

            let need_unwatch = dict_watch_count(dict) == 0;
            if unlikely(need_unwatch && G_DICT_WATCHER_REGISTERED) {
                let _ = PyDict_Unwatch(G_DICT_WATCHER_ID, dict);
            }

            self.prev = ptr::null_mut();
            self.next = ptr::null_mut();
        }

        #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
        unsafe {
            if unlikely(!self.active) {
                return;
            }
            self.active = false;
            if likely(!self.it.is_null()) {
                self.it.decref();
                self.it = ptr::null_mut();
            }
        }
    }

    /// Returns 1 if yielded (key/value are new refs), 0 if exhausted, -1 on error.
    #[inline(always)]
    pub unsafe fn next(&mut self, key: &mut *mut PyObject, value: &mut *mut PyObject) -> i32 {
        *key = ptr::null_mut();
        *value = ptr::null_mut();

        #[cfg(not(Py_3_14))]
        unsafe {
            let mut k: *mut PyObject = ptr::null_mut();
            let mut v: *mut PyObject = ptr::null_mut();

            if likely(PyDict_Next(self.dict, &mut self.pos, &mut k, &mut v) != 0) {
                let ver_now = dict_version_tag(self.dict);
                if unlikely(ver_now != self.ver0) {
                    let used_now = dict_used(self.dict);
                    if unlikely(used_now != self.used0) {
                        PyErr_SetString(
                            PyExc_RuntimeError,
                            crate::cstr!("dictionary changed size during iteration"),
                        );
                    } else {
                        PyErr_SetString(
                            PyExc_RuntimeError,
                            crate::cstr!("dictionary keys changed during iteration"),
                        );
                    }
                    return -1;
                }

                k.incref();
                v.incref();
                *key = k;
                *value = v;
                return 1;
            }

            0
        }

        #[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
        unsafe {
            let mut k: *mut PyObject = ptr::null_mut();
            let mut v: *mut PyObject = ptr::null_mut();

            if likely(PyDict_Next(self.dict, &mut self.pos, &mut k, &mut v) != 0) {
                if unlikely(self.mutated) {
                    let cur = PyDict_Size(self.dict);
                    let size_changed_now = if likely(cur >= 0) {
                        cur != self.size0
                    } else {
                        self.size_changed
                    };

                    PyErr_SetString(
                        PyExc_RuntimeError,
                        if unlikely(size_changed_now) {
                            crate::cstr!("dictionary changed size during iteration")
                        } else {
                            crate::cstr!("dictionary keys changed during iteration")
                        },
                    );
                    self.cleanup();
                    return -1;
                }

                k.incref();
                v.incref();
                *key = k;
                *value = v;
                return 1;
            }

            if unlikely(self.mutated) {
                let cur = PyDict_Size(self.dict);
                let size_changed_now = if likely(cur >= 0) {
                    cur != self.size0
                } else {
                    self.size_changed
                };

                PyErr_SetString(
                    PyExc_RuntimeError,
                    if unlikely(size_changed_now) {
                        crate::cstr!("dictionary changed size during iteration")
                    } else {
                        crate::cstr!("dictionary keys changed during iteration")
                    },
                );
                self.cleanup();
                return -1;
            }

            self.cleanup();
            0
        }

        #[cfg(all(Py_3_14, Py_GIL_DISABLED))]
        unsafe {
            if unlikely(self.it.is_null()) {
                return -1;
            }

            let mut item: *mut PyObject = ptr::null_mut();
            let result = PyIter_NextItem(self.it, &mut item);

            if unlikely(result != 1) {
                self.cleanup();
                return result;
            }

            if unlikely(item.is_null() || PyTuple_Check(item) == 0 || PyTuple_GET_SIZE(item) != 2) {
                item.decref_nullable();
                PyErr_SetString(
                    PyExc_RuntimeError,
                    crate::cstr!("dict.items() iterator returned non-pair item"),
                );
                self.cleanup();
                return -1;
            }

            *key = PyTuple_GET_ITEM(item, 0).newref();
            *value = PyTuple_GET_ITEM(item, 1).newref();
            item.decref();
            1
        }
    }
}

impl Drop for DictIterGuard {
    fn drop(&mut self) {
        unsafe {
            self.cleanup();
        }
    }
}

#[cfg(not(Py_3_14))]
#[inline(always)]
pub unsafe fn dict_iter_module_init() -> i32 {
    0
}

#[cfg(not(Py_3_14))]
#[inline(always)]
pub unsafe fn dict_iter_module_cleanup() {}

#[cfg(all(Py_3_14, Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn dict_iter_module_init() -> i32 {
    0
}

#[cfg(all(Py_3_14, Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn dict_iter_module_cleanup() {}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
pub unsafe fn dict_iter_module_init() -> i32 {
    if likely(unsafe { !G_DICT_WATCHER_REGISTERED }) {
        let watcher_id = unsafe { PyDict_AddWatcher(Some(copium_dict_watcher_cb)) };
        if unlikely(watcher_id < 0) {
            return -1;
        }

        unsafe {
            G_DICT_WATCHER_ID = watcher_id;
            G_DICT_WATCHER_REGISTERED = true;
        }
    }

    0
}

#[cfg(all(Py_3_14, not(Py_GIL_DISABLED)))]
pub unsafe fn dict_iter_module_cleanup() {
    if likely(unsafe { G_DICT_WATCHER_REGISTERED }) {
        let _ = unsafe { PyDict_ClearWatcher(G_DICT_WATCHER_ID) };
        unsafe {
            G_DICT_WATCHER_REGISTERED = false;
            G_DICT_WATCHER_ID = -1;
        }
    }

    unsafe {
        G_GUARD_LIST_HEAD = ptr::null_mut();
    }
}
