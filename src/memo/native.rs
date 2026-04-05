use super::{KeepaliveVec, Memo, MemoCheckpoint, MemoTable, UndoLog};
use crate::memo::table::{hash_pointer, TOMBSTONE};
use crate::py::{self, *};
use std::ffi::c_void;
use std::hint::unlikely;
use std::ptr;

#[repr(C)]
pub struct PyMemoObject {
    pub ob_base: PyObject,
    pub table: MemoTable,
    pub keepalive: KeepaliveVec,
    pub undo_log: UndoLog,
    pub dict_proxy: *mut PyObject,
}

impl PyMemoObject {
    pub fn init_in_place(&mut self) {
        unsafe {
            ptr::write(ptr::addr_of_mut!(self.table), MemoTable::new());
            ptr::write(ptr::addr_of_mut!(self.keepalive), KeepaliveVec::new());
            ptr::write(ptr::addr_of_mut!(self.undo_log), UndoLog::new());
            ptr::write(ptr::addr_of_mut!(self.dict_proxy), ptr::null_mut());
        }
    }

    pub fn reset(&mut self) {
        self.keepalive.clear();
        self.keepalive.shrink_if_large();
        self.undo_log.clear();
        self.undo_log.shrink_if_large();
        self.table.reset();
        if !self.dict_proxy.is_null() {
            unsafe { self.dict_proxy.decref() };
            self.dict_proxy = ptr::null_mut();
        }
    }

    #[inline(always)]
    pub fn checkpoint(&self) -> MemoCheckpoint {
        self.undo_log.keys.len()
    }

    #[cold]
    pub fn rollback(&mut self, cp: MemoCheckpoint) {
        for i in cp..self.undo_log.keys.len() {
            let key = self.undo_log.keys[i];
            let hash = hash_pointer(key);
            let _ = self.table.remove_h(key, hash);
        }
        self.undo_log.keys.truncate(cp);
    }

    pub(crate) fn insert_logged(&mut self, key: usize, value: *mut PyObject, hash: usize) -> i32 {
        if self.table.insert_h(key, value, hash) < 0 {
            return -1;
        }
        self.undo_log.append(key);
        0
    }

    #[cold]
    pub unsafe fn to_dict(&self) -> *mut PyDictObject {
        unsafe {
            let dict = py::dict::new_presized(0);
            if dict.is_null() {
                return ptr::null_mut();
            }

            if self.table.slots.is_null() {
                return dict;
            }

            for i in 0..self.table.size {
                let entry = &*self.table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let pykey = py::long::from_ptr(entry.key as *mut c_void);
                    if pykey.is_null() {
                        dict.decref();
                        return ptr::null_mut();
                    }
                    if dict.set_item(pykey, entry.value) < 0 {
                        pykey.decref();
                        dict.decref();
                        return ptr::null_mut();
                    }
                    pykey.decref();
                }
            }

            dict
        }
    }

    #[cold]
    pub unsafe fn sync_from_dict(&mut self, dict: *mut PyDictObject, orig_size: Py_ssize_t) -> i32 {
        unsafe {
            let cur_size = dict.size();
            if cur_size <= orig_size {
                return 0;
            }

            let mut pos: Py_ssize_t = 0;
            let mut py_key: *mut PyObject = ptr::null_mut();
            let mut value: *mut PyObject = ptr::null_mut();
            let mut idx: Py_ssize_t = 0;

            while dict.dict_next(&mut pos, &mut py_key, &mut value) {
                idx += 1;
                if idx <= orig_size {
                    continue;
                }
                if !py_key.is_long() {
                    continue;
                }
                let key = py_key.as_void_ptr() as usize;
                if key == 0 && !py::err::occurred().is_null() {
                    return -1;
                }
                let hash = hash_pointer(key);
                if self.table.insert_h(key, value, hash) < 0 {
                    return -1;
                }
            }

            0
        }
    }
}

impl Memo for PyMemoObject {
    type Probe = usize;
    const RECALL_CAN_ERROR: bool = false;

    #[inline(always)]
    unsafe fn recall<T: PyTypeInfo>(&mut self, object: *mut T) -> (usize, *mut T) {
        let key = object as usize;
        let hash = hash_pointer(key);
        let found = self.recall_probed(object, &hash);
        (hash, found)
    }

    #[inline(always)]
    unsafe fn recall_probed<T: PyTypeInfo>(
        &mut self,
        object: *mut T,
        probe: &Self::Probe,
    ) -> *mut T {
        let key = object as usize;
        let found = self.table.lookup_h(key, *probe);
        if !found.is_null() {
            unsafe { found.incref() };
        }
        T::cast_unchecked(found)
    }

    #[inline(always)]
    unsafe fn memoize<T: PyTypeInfo>(
        &mut self,
        original: *mut T,
        copy: *mut T,
        probe: &Self::Probe,
    ) -> i32 {
        let key = original as usize;
        if unlikely(self.table.insert_h(key, copy.cast(), *probe) < 0) {
            return -1;
        }
        self.keepalive.append(original);
        0
    }

    #[cold]
    unsafe fn forget<T: PyTypeInfo>(&mut self, original: *mut T, probe: &Self::Probe) {
        let _ = self.table.remove_h(original as usize, *probe);
    }

    unsafe fn as_call_arg(&mut self) -> *mut PyObject {
        std::ptr::from_mut(self).cast()
    }

    unsafe fn checkpoint(&mut self) -> Option<MemoCheckpoint> {
        Some(PyMemoObject::checkpoint(self))
    }

    unsafe fn as_native_memo(&mut self) -> *mut PyMemoObject {
        self
    }
}

impl Drop for PyMemoObject {
    fn drop(&mut self) {
        self.reset();
    }
}
