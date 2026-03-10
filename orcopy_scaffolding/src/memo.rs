use pyo3_ffi::*;
use std::ffi::c_void;
use std::ptr;

use crate::state::STATE;
use crate::types::*;

// ══════════════════════════════════════════════════════════════
//  Memo trait — unified interface, three backends
// ══════════════════════════════════════════════════════════════

pub trait Memo: Sized {
    type Probe;

    const RECALL_CAN_ERROR: bool;

    /// Look up object in memo. Returns (probe_for_later, found_or_null).
    /// When found is non-null, it is a new reference.
    /// When RECALL_CAN_ERROR is true, caller must check PyErr_Occurred() on null.
    unsafe fn recall(&mut self, object: *mut PyObject) -> (Self::Probe, *mut PyObject);

    /// Store (original → copy) mapping. Returns 0 on success, -1 on error.
    unsafe fn memoize(
        &mut self,
        original: *mut PyObject,
        copy: *mut PyObject,
        probe: &Self::Probe,
    ) -> i32;

    /// Remove a mapping (used on error paths in native memo).
    unsafe fn forget(&mut self, original: *mut PyObject, probe: &Self::Probe);

    /// Get the Python object to pass to __deepcopy__(memo).
    unsafe fn as_call_arg(&mut self) -> *mut PyObject;

    /// Ensure any keepalive-backed memo state is ready before a copy path that
    /// needs stdlib-compatible early memo interaction.
    #[inline(always)]
    unsafe fn ensure_memo_is_valid(&mut self) -> i32 {
        0
    }

    #[inline(always)]
    unsafe fn checkpoint(&mut self) -> Option<MemoCheckpoint> {
        None
    }

    #[inline(always)]
    unsafe fn as_native_memo(&mut self) -> *mut PyMemoObject {
        ptr::null_mut()
    }
}

// ══════════════════════════════════════════════════════════════
//  MemoTable — pointer-keyed open-addressing hash table
// ══════════════════════════════════════════════════════════════

pub(crate) const TOMBSTONE: usize = usize::MAX;

const MEMO_RETAIN_MAX_SLOTS: usize = 1 << 17;
const MEMO_RETAIN_SHRINK_TO: usize = 1 << 13;
const KEEP_RETAIN_MAX: usize = 1 << 13;
const KEEP_RETAIN_TARGET: usize = 1 << 10;

pub(crate) struct MemoEntry {
    pub(crate) key: usize,
    pub(crate) value: *mut PyObject,
}

pub struct MemoTable {
    pub(crate) slots: *mut MemoEntry,
    pub(crate) size: usize,
    pub(crate) used: usize,
    pub(crate) filled: usize,
}

#[inline(always)]
pub(crate) fn hash_pointer(ptr: usize) -> usize {
    let mut h = ptr;
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h = h.wrapping_mul(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;
    h
}

impl MemoTable {
    fn new() -> Self {
        Self {
            slots: ptr::null_mut(),
            size: 0,
            used: 0,
            filled: 0,
        }
    }

    fn ensure(&mut self) -> i32 {
        if !self.slots.is_null() {
            return 0;
        }
        self.resize(1)
    }

    fn resize(&mut self, min_needed: usize) -> i32 {
        let mut new_size = 8usize;
        while new_size < min_needed.saturating_mul(2) {
            new_size = new_size.saturating_mul(2);
        }

        let layout = std::alloc::Layout::array::<MemoEntry>(new_size).unwrap();
        let new_slots = unsafe { std::alloc::alloc_zeroed(layout) as *mut MemoEntry };
        if new_slots.is_null() {
            return -1;
        }

        let old_slots = self.slots;
        let old_size = self.size;

        self.slots = new_slots;
        self.size = new_size;
        self.used = 0;
        self.filled = 0;

        if !old_slots.is_null() {
            for i in 0..old_size {
                let entry = unsafe { &*old_slots.add(i) };
                if entry.key != 0 && entry.key != TOMBSTONE {
                    self.insert_no_grow(entry.key, entry.value);
                }
            }
            let old_layout = std::alloc::Layout::array::<MemoEntry>(old_size).unwrap();
            unsafe { std::alloc::dealloc(old_slots as *mut u8, old_layout) };
        }

        0
    }

    fn insert_no_grow(&mut self, key: usize, value: *mut PyObject) {
        let mask = self.size - 1;
        let mut idx = hash_pointer(key) & mask;

        loop {
            let entry = unsafe { &mut *self.slots.add(idx) };
            if entry.key == 0 {
                entry.key = key;
                entry.value = value;
                unsafe { Py_INCREF(value) };
                self.used += 1;
                self.filled += 1;
                return;
            }
            if entry.key == key {
                let old = entry.value;
                entry.value = value;
                unsafe {
                    Py_INCREF(value);
                    Py_XDECREF(old);
                }
                return;
            }
            idx = (idx + 1) & mask;
        }
    }

    pub fn lookup_h(&self, key: usize, hash: usize) -> *mut PyObject {
        if self.slots.is_null() {
            return ptr::null_mut();
        }

        let mask = self.size - 1;
        let mut idx = hash & mask;

        loop {
            let entry = unsafe { &*self.slots.add(idx) };
            if entry.key == 0 {
                return ptr::null_mut();
            }
            if entry.key != TOMBSTONE && entry.key == key {
                return entry.value; // borrowed
            }
            idx = (idx + 1) & mask;
        }
    }

    pub fn insert_h(&mut self, key: usize, value: *mut PyObject, hash: usize) -> i32 {
        if self.ensure() < 0 {
            return -1;
        }
        if self.filled * 10 >= self.size * 7 {
            if self.resize(self.used + 1) < 0 {
                return -1;
            }
        }

        let mask = self.size - 1;
        let mut idx = hash & mask;
        let mut first_tomb: Option<usize> = None;

        loop {
            let entry = unsafe { &mut *self.slots.add(idx) };
            if entry.key == 0 {
                let at = first_tomb.unwrap_or(idx);
                let slot = unsafe { &mut *self.slots.add(at) };
                slot.key = key;
                unsafe { Py_INCREF(value) };
                slot.value = value;
                self.used += 1;
                self.filled += 1;
                return 0;
            }
            if entry.key == TOMBSTONE {
                if first_tomb.is_none() {
                    first_tomb = Some(idx);
                }
            } else if entry.key == key {
                let old = entry.value;
                unsafe {
                    Py_INCREF(value);
                    entry.value = value;
                    Py_XDECREF(old);
                }
                return 0;
            }
            idx = (idx + 1) & mask;
        }
    }

    pub fn remove_h(&mut self, key: usize, hash: usize) -> i32 {
        if self.slots.is_null() {
            return -1;
        }

        let mask = self.size - 1;
        let mut idx = hash & mask;

        loop {
            let entry = unsafe { &mut *self.slots.add(idx) };
            if entry.key == 0 {
                return -1;
            }
            if entry.key != TOMBSTONE && entry.key == key {
                entry.key = TOMBSTONE;
                unsafe { Py_XDECREF(entry.value) };
                entry.value = ptr::null_mut();
                self.used -= 1;
                return 0;
            }
            idx = (idx + 1) & mask;
        }
    }

    pub fn clear(&mut self) {
        if self.slots.is_null() {
            return;
        }

        for i in 0..self.size {
            let entry = unsafe { &*self.slots.add(i) };
            if entry.key != 0 && entry.key != TOMBSTONE {
                unsafe { Py_XDECREF(entry.value) };
            }
        }
        unsafe { ptr::write_bytes(self.slots, 0, self.size) };
        self.used = 0;
        self.filled = 0;
    }

    pub fn reset(&mut self) {
        self.clear();
        if self.size > MEMO_RETAIN_MAX_SLOTS {
            let _ = self.resize(MEMO_RETAIN_SHRINK_TO / 2);
        }
    }
}

impl Drop for MemoTable {
    fn drop(&mut self) {
        if self.slots.is_null() {
            return;
        }

        for i in 0..self.size {
            let entry = unsafe { &*self.slots.add(i) };
            if entry.key != 0 && entry.key != TOMBSTONE {
                unsafe { Py_XDECREF(entry.value) };
            }
        }

        let layout = std::alloc::Layout::array::<MemoEntry>(self.size).unwrap();
        unsafe { std::alloc::dealloc(self.slots as *mut u8, layout) };
    }
}

// ══════════════════════════════════════════════════════════════
//  KeepaliveVector
// ══════════════════════════════════════════════════════════════

pub struct KeepaliveVec {
    pub(crate) items: Vec<*mut PyObject>,
}

impl KeepaliveVec {
    fn new() -> Self {
        Self { items: Vec::new() }
    }

    pub fn append(&mut self, obj: *mut PyObject) {
        unsafe { Py_INCREF(obj) };
        self.items.push(obj);
    }

    pub fn clear(&mut self) {
        for &item in &self.items {
            unsafe { Py_DECREF(item) };
        }
        self.items.clear();
    }
}

impl KeepaliveVec {
    pub fn shrink_if_large(&mut self) {
        if self.items.capacity() > KEEP_RETAIN_MAX {
            self.items.shrink_to(KEEP_RETAIN_TARGET);
        }
    }
}

impl Drop for KeepaliveVec {
    fn drop(&mut self) {
        self.clear();
    }
}

// ══════════════════════════════════════════════════════════════
//  UndoLog — tracks keys for rollback
// ══════════════════════════════════════════════════════════════

pub struct UndoLog {
    keys: Vec<usize>,
}

impl UndoLog {
    fn new() -> Self {
        Self { keys: Vec::new() }
    }

    fn append(&mut self, key: usize) {
        self.keys.push(key);
    }

    fn clear(&mut self) {
        self.keys.clear();
    }

    fn shrink_if_large(&mut self) {
        if self.keys.capacity() > KEEP_RETAIN_MAX {
            self.keys.shrink_to(KEEP_RETAIN_TARGET);
        }
    }
}

pub type MemoCheckpoint = usize;

// ══════════════════════════════════════════════════════════════
//  Native memo Python object
// ══════════════════════════════════════════════════════════════

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
            unsafe { Py_DECREF(self.dict_proxy) };
            self.dict_proxy = ptr::null_mut();
        }
    }

    pub fn checkpoint(&self) -> MemoCheckpoint {
        self.undo_log.keys.len()
    }

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

    /// Export table contents to a Python dict for __deepcopy__ fallback.
    pub unsafe fn to_dict(&self) -> *mut PyObject {
        unsafe {
            let dict = PyDict_New();
            if dict.is_null() {
                return ptr::null_mut();
            }

            if self.table.slots.is_null() {
                return dict;
            }

            for i in 0..self.table.size {
                let entry = &*self.table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let pykey = PyLong_FromVoidPtr(entry.key as *mut c_void);
                    if pykey.is_null() {
                        Py_DECREF(dict);
                        return ptr::null_mut();
                    }
                    if PyDict_SetItem(dict, pykey, entry.value) < 0 {
                        Py_DECREF(pykey);
                        Py_DECREF(dict);
                        return ptr::null_mut();
                    }
                    Py_DECREF(pykey);
                }
            }

            dict
        }
    }

    /// Sync new entries from dict back into table.
    pub unsafe fn sync_from_dict(&mut self, dict: *mut PyObject, orig_size: Py_ssize_t) -> i32 {
        unsafe {
            let cur_size = PyDict_Size(dict);
            if cur_size <= orig_size {
                return 0;
            }

            let mut pos: Py_ssize_t = 0;
            let mut py_key: *mut PyObject = ptr::null_mut();
            let mut value: *mut PyObject = ptr::null_mut();
            let mut idx: Py_ssize_t = 0;

            while PyDict_Next(dict, &mut pos, &mut py_key, &mut value) != 0 {
                idx += 1;
                if idx <= orig_size {
                    continue;
                }
                if PyLong_Check(py_key) == 0 {
                    continue;
                }
                let key = PyLong_AsVoidPtr(py_key) as usize;
                if key == 0 && !PyErr_Occurred().is_null() {
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
    unsafe fn recall(&mut self, object: *mut PyObject) -> (usize, *mut PyObject) {
        let key = object as usize;
        let hash = hash_pointer(key);
        let found = self.table.lookup_h(key, hash);
        if !found.is_null() {
            unsafe { Py_INCREF(found) };
        }
        (hash, found)
    }

    #[inline(always)]
    unsafe fn memoize(
        &mut self,
        original: *mut PyObject,
        copy: *mut PyObject,
        probe: &usize,
    ) -> i32 {
        let key = original as usize;
        if self.table.insert_h(key, copy, *probe) < 0 {
            return -1;
        }
        self.keepalive.append(original);
        0
    }

    #[inline(always)]
    unsafe fn forget(&mut self, original: *mut PyObject, probe: &usize) {
        let _ = self.table.remove_h(original as usize, *probe);
    }

    unsafe fn as_call_arg(&mut self) -> *mut PyObject {
        self as *mut PyMemoObject as *mut PyObject
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

pub static mut Memo_Type: PyTypeObject = unsafe { std::mem::zeroed() };
static mut MEMO_MAPPING: PyMappingMethods = unsafe { std::mem::zeroed() };
static mut MEMO_SEQUENCE: PySequenceMethods = unsafe { std::mem::zeroed() };
static mut MEMO_METHODS_TABLE: [PyMethodDef; 9] = unsafe { std::mem::zeroed() };

#[repr(C)]
struct PyKeepaliveListObject {
    ob_base: PyObject,
    owner: *mut PyMemoObject,
}

static mut KEEPALIVE_LIST_TYPE: PyTypeObject = unsafe { std::mem::zeroed() };
static mut KEEPALIVE_LIST_SEQUENCE: PySequenceMethods = unsafe { std::mem::zeroed() };
static mut KEEPALIVE_LIST_METHODS_TABLE: [PyMethodDef; 3] = unsafe { std::mem::zeroed() };

unsafe fn keepalive_list_new(owner: *mut PyMemoObject) -> *mut PyObject {
    unsafe {
        let obj = PyObject_GC_New::<PyKeepaliveListObject>(ptr::addr_of_mut!(KEEPALIVE_LIST_TYPE));
        if obj.is_null() {
            return ptr::null_mut();
        }
        (*obj).owner = owner;
        Py_INCREF(owner as *mut PyObject);
        PyObject_GC_Track(obj as *mut c_void);
        obj as *mut PyObject
    }
}

unsafe fn memo_keepalive_proxy(self_: *mut PyMemoObject) -> *mut PyObject {
    unsafe {
        if !(*self_).dict_proxy.is_null() {
            return Py_NewRef((*self_).dict_proxy);
        }

        let proxy = keepalive_list_new(self_);
        if proxy.is_null() {
            return ptr::null_mut();
        }

        (*self_).dict_proxy = proxy;
        Py_NewRef(proxy)
    }
}

unsafe extern "C" fn keepalive_list_dealloc(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        PyObject_GC_UnTrack(self_ as *mut c_void);
        Py_XDECREF((*self_).owner as *mut PyObject);
        PyObject_GC_Del(self_ as *mut c_void);
    }
}

unsafe extern "C" fn keepalive_list_traverse(
    obj: *mut PyObject,
    visit: visitproc,
    arg: *mut c_void,
) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if !(*self_).owner.is_null() {
            return visit((*self_).owner as *mut PyObject, arg);
        }
        0
    }
}

unsafe extern "C" fn keepalive_list_clear(obj: *mut PyObject) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        Py_CLEAR(&mut (*self_).owner as *mut *mut PyMemoObject as *mut *mut PyObject);
        0
    }
}

unsafe extern "C" fn keepalive_list_len(obj: *mut PyObject) -> Py_ssize_t {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            return 0;
        }
        (*(*self_).owner).keepalive.items.len() as Py_ssize_t
    }
}

unsafe extern "C" fn keepalive_list_getitem(
    obj: *mut PyObject,
    index: Py_ssize_t,
) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            PyErr_SetString(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }

        let items = &(*(*self_).owner).keepalive.items;
        let n = items.len() as Py_ssize_t;
        let mut i = index;
        if i < 0 {
            i += n;
        }
        if i < 0 || i >= n {
            PyErr_SetString(PyExc_IndexError, cstr!("index out of range"));
            return ptr::null_mut();
        }

        Py_NewRef(items[i as usize])
    }
}

unsafe extern "C" fn keepalive_list_iter(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            PyErr_SetString(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }

        let items = &(*(*self_).owner).keepalive.items;
        let n = items.len() as Py_ssize_t;
        let list = PyList_New(n);
        if list.is_null() {
            return ptr::null_mut();
        }

        for (i, &item) in items.iter().enumerate() {
            Py_INCREF(item);
            PyList_SetItem(list, i as Py_ssize_t, item);
        }

        let it = PyObject_GetIter(list);
        Py_DECREF(list);
        it
    }
}

unsafe extern "C" fn keepalive_list_repr(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let list = PySequence_List(obj);
        if list.is_null() {
            return ptr::null_mut();
        }
        let inner = PyObject_Repr(list);
        Py_DECREF(list);
        if inner.is_null() {
            return ptr::null_mut();
        }
        let wrapped = PyUnicode_FromFormat(cstr!("keepalive(%U)"), inner);
        Py_DECREF(inner);
        wrapped
    }
}

unsafe extern "C" fn keepalive_list_append(obj: *mut PyObject, arg: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            PyErr_SetString(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }
        (*(*self_).owner).keepalive.append(arg);
        Py_NewRef(Py_None())
    }
}

unsafe extern "C" fn keepalive_list_clear_py(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            PyErr_SetString(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }
        (*(*self_).owner).keepalive.clear();
        Py_NewRef(Py_None())
    }
}

unsafe fn init_keepalive_methods() {
    unsafe {
        KEEPALIVE_LIST_METHODS_TABLE[0] = PyMethodDef {
            ml_name: cstr!("append"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: keepalive_list_append,
            },
            ml_flags: METH_O,
            ml_doc: ptr::null(),
        };
        KEEPALIVE_LIST_METHODS_TABLE[1] = PyMethodDef {
            ml_name: cstr!("clear"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: keepalive_list_clear_py,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        KEEPALIVE_LIST_METHODS_TABLE[2] = PyMethodDef::zeroed();

        KEEPALIVE_LIST_SEQUENCE = PySequenceMethods {
            sq_length: Some(keepalive_list_len),
            sq_concat: None,
            sq_repeat: None,
            sq_item: Some(keepalive_list_getitem),
            was_sq_slice: ptr::null_mut(),
            sq_ass_item: None,
            was_sq_ass_slice: ptr::null_mut(),
            sq_contains: None,
            sq_inplace_concat: None,
            sq_inplace_repeat: None,
        };

        let tp = ptr::addr_of_mut!(KEEPALIVE_LIST_TYPE);
        (*tp).tp_name = cstr!("copium.keepalive");
        (*tp).tp_basicsize = std::mem::size_of::<PyKeepaliveListObject>() as Py_ssize_t;
        (*tp).tp_dealloc = Some(keepalive_list_dealloc);
        (*tp).tp_repr = Some(keepalive_list_repr);
        (*tp).tp_as_sequence = ptr::addr_of_mut!(KEEPALIVE_LIST_SEQUENCE);
        #[cfg(Py_GIL_DISABLED)]
        {
            (*tp).tp_flags.store(Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, core::sync::atomic::Ordering::Relaxed);
        }
        #[cfg(not(Py_GIL_DISABLED))]
        {
            (*tp).tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
        }
        (*tp).tp_traverse = Some(keepalive_list_traverse);
        (*tp).tp_clear = Some(keepalive_list_clear);
        (*tp).tp_iter = Some(keepalive_list_iter);
        (*tp).tp_methods = KEEPALIVE_LIST_METHODS_TABLE.as_mut_ptr();
    }
}

unsafe impl PyTypeInfo for PyMemoObject {
    fn type_ptr() -> *mut PyTypeObject {
        unsafe { ptr::addr_of_mut!(Memo_Type) }
    }
}

// ── TSS ────────────────────────────────────────────────────

#[thread_local]
static mut TSS_MEMO: *mut PyMemoObject = ptr::null_mut();

pub unsafe fn pymemo_alloc() -> *mut PyMemoObject {
    unsafe {
        let memo = PyObject_GC_New::<PyMemoObject>(ptr::addr_of_mut!(Memo_Type));
        if memo.is_null() {
            return ptr::null_mut();
        }

        (*memo).init_in_place();
        memo
    }
}

pub unsafe fn get_memo() -> (*mut PyMemoObject, bool) {
    unsafe {
        let tss = TSS_MEMO;
        if tss.is_null() {
            let fresh = pymemo_alloc();
            if fresh.is_null() {
                return (ptr::null_mut(), false);
            }
            TSS_MEMO = fresh;
            return (fresh, true);
        }

        if Py_REFCNT(tss as *mut PyObject) == 1 {
            return (tss, true);
        }

        (pymemo_alloc(), false)
    }
}

pub unsafe fn cleanup_memo(memo: *mut PyMemoObject, is_tss: bool) {
    unsafe {
        if is_tss && Py_REFCNT(memo as *mut PyObject) == 1 {
            (*memo).reset();
            return;
        }

        if is_tss {
            let fresh = pymemo_alloc();
            if fresh.is_null() {
                TSS_MEMO = ptr::null_mut();
            } else {
                TSS_MEMO = fresh;
            }
        }

        PyObject_GC_Track(memo as *mut c_void);
        Py_DECREF(memo as *mut PyObject);
    }
}

// ══════════════════════════════════════════════════════════════
//  DictMemo — wraps a PyDict, matching legacy dict path
// ══════════════════════════════════════════════════════════════

pub struct DictMemo {
    pub dict: *mut PyDictObject,
    keepalive: *mut PyListObject, // lazily initialized list at dict[id(dict)]
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
                Py_INCREF(existing);
                self.keepalive = existing as *mut PyListObject;
                Py_DECREF(pykey);
                return 0;
            }
            if !PyErr_Occurred().is_null() {
                Py_DECREF(pykey);
                return -1;
            }

            let list = py_list_new(0);
            if list.is_null() {
                Py_DECREF(pykey);
                return -1;
            }

            if self.dict.set_item(pykey, list as *mut PyObject) < 0 {
                list.decref();
                Py_DECREF(pykey);
                return -1;
            }

            self.keepalive = list as *mut PyListObject; // dict owns one ref, we own one ref
            Py_DECREF(pykey);
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
                Py_INCREF(found);
            }
            Py_DECREF(pykey);
            ((), found)
        }
    }

    #[inline(always)]
    unsafe fn memoize(
        &mut self,
        original: *mut PyObject,
        copy: *mut PyObject,
        _probe: &(),
    ) -> i32 {
        unsafe {
            let pykey = PyLong_FromVoidPtr(original as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let rc = self.dict.set_item(pykey, copy);
            Py_DECREF(pykey);
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

// ══════════════════════════════════════════════════════════════
//  AnyMemo — arbitrary Python mapping (non-dict, non-native)
// ══════════════════════════════════════════════════════════════

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

            let s = &STATE;
            let pykey = PyLong_FromVoidPtr(self.object as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let existing = PyObject_CallMethodObjArgs(
                self.object,
                s.s_get,
                pykey,
                s.sentinel,
                ptr::null_mut::<PyObject>(),
            );
            if existing.is_null() {
                Py_DECREF(pykey);
                return -1;
            }

            if existing != s.sentinel {
                self.keepalive = existing;
                Py_DECREF(pykey);
                return 0;
            }

            Py_DECREF(existing);

            let list = PyList_New(0);
            if list.is_null() {
                Py_DECREF(pykey);
                return -1;
            }

            if PyObject_SetItem(self.object, pykey, list) < 0 {
                Py_DECREF(list);
                Py_DECREF(pykey);
                return -1;
            }

            self.keepalive = list;
            Py_DECREF(pykey);
            0
        }
    }
}

impl Memo for AnyMemo {
    type Probe = ();
    const RECALL_CAN_ERROR: bool = true;

    unsafe fn recall(&mut self, object: *mut PyObject) -> ((), *mut PyObject) {
        unsafe {
            let s = &STATE;
            let pykey = PyLong_FromVoidPtr(object as *mut c_void);
            if pykey.is_null() {
                return ((), ptr::null_mut());
            }

            let found = PyObject_CallMethodObjArgs(
                self.object,
                s.s_get,
                pykey,
                s.sentinel,
                ptr::null_mut::<PyObject>(),
            );
            Py_DECREF(pykey);

            if found.is_null() {
                return ((), ptr::null_mut()); // error set
            }
            if found == s.sentinel {
                Py_DECREF(found);
                return ((), ptr::null_mut()); // not found, no error
            }

            ((), found) // new ref
        }
    }

    unsafe fn memoize(
        &mut self,
        original: *mut PyObject,
        copy: *mut PyObject,
        _probe: &(),
    ) -> i32 {
        unsafe {
            let pykey = PyLong_FromVoidPtr(original as *mut c_void);
            if pykey.is_null() {
                return -1;
            }

            let rc = PyObject_SetItem(self.object, pykey, copy);
            Py_DECREF(pykey);
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
            unsafe { Py_DECREF(self.keepalive) };
        }
    }
}

// ── tp_dealloc / tp_finalize / tp_traverse / tp_clear ──────

unsafe extern "C" fn memo_dealloc(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        PyObject_GC_UnTrack(self_ as *mut c_void);
        if PyObject_CallFinalizerFromDealloc(obj) != 0 {
            return;
        }
        ptr::drop_in_place(self_);
        PyObject_GC_Del(self_ as *mut c_void);
    }
}

unsafe extern "C" fn memo_finalize(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).table.clear();
        (*self_).keepalive.clear();
        if !(*self_).dict_proxy.is_null() {
            Py_DECREF((*self_).dict_proxy);
            (*self_).dict_proxy = ptr::null_mut();
        }
    }
}

unsafe extern "C" fn memo_traverse(
    obj: *mut PyObject,
    visit: visitproc,
    arg: *mut c_void,
) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let inner = &*self_;

        if !inner.table.slots.is_null() {
            for i in 0..inner.table.size {
                let entry = &*inner.table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE && !entry.value.is_null() {
                    let rc = visit(entry.value, arg);
                    if rc != 0 {
                        return rc;
                    }
                }
            }
        }

        for &item in &inner.keepalive.items {
            if !item.is_null() {
                let rc = visit(item, arg);
                if rc != 0 {
                    return rc;
                }
            }
        }

        if !inner.dict_proxy.is_null() {
            let rc = visit(inner.dict_proxy, arg);
            if rc != 0 {
                return rc;
            }
        }

        0
    }
}

unsafe extern "C" fn memo_clear_gc(obj: *mut PyObject) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).table.clear();
        (*self_).keepalive.clear();
        if !(*self_).dict_proxy.is_null() {
            Py_DECREF((*self_).dict_proxy);
            (*self_).dict_proxy = ptr::null_mut();
        }
        0
    }
}

// ── tp_repr / tp_iter ──────────────────────────────────────

unsafe extern "C" fn memo_repr(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let dict = (*self_).to_dict();
        if dict.is_null() {
            return ptr::null_mut();
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = PyLong_FromVoidPtr(self_ as *mut c_void);
            if py_key.is_null() {
                Py_DECREF(dict);
                return ptr::null_mut();
            }

            let proxy = memo_keepalive_proxy(self_);
            if proxy.is_null() {
                Py_DECREF(py_key);
                Py_DECREF(dict);
                return ptr::null_mut();
            }

            if PyDict_SetItem(dict, py_key, proxy) < 0 {
                Py_DECREF(proxy);
                Py_DECREF(py_key);
                Py_DECREF(dict);
                return ptr::null_mut();
            }

            Py_DECREF(proxy);
            Py_DECREF(py_key);
        }

        let inner = PyObject_Repr(dict);
        Py_DECREF(dict);
        if inner.is_null() {
            return ptr::null_mut();
        }

        let wrapped = PyUnicode_FromFormat(cstr!("memo(%U)"), inner);
        Py_DECREF(inner);
        wrapped
    }
}

unsafe extern "C" fn memo_iter(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let table = &(*self_).table;

        let list = PyList_New(0);
        if list.is_null() {
            return ptr::null_mut();
        }

        if !table.slots.is_null() {
            for i in 0..table.size {
                let entry = &*table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let py_key = PyLong_FromVoidPtr(entry.key as *mut c_void);
                    if py_key.is_null() || PyList_Append(list, py_key) < 0 {
                        Py_XDECREF(py_key);
                        Py_DECREF(list);
                        return ptr::null_mut();
                    }
                    Py_DECREF(py_key);
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = PyLong_FromVoidPtr(self_ as *mut c_void);
            if py_key.is_null() || PyList_Append(list, py_key) < 0 {
                Py_XDECREF(py_key);
                Py_DECREF(list);
                return ptr::null_mut();
            }
            Py_DECREF(py_key);
        }

        let it = PyObject_GetIter(list);
        Py_DECREF(list);
        it
    }
}

// ── Mapping protocol ───────────────────────────────────────

unsafe extern "C" fn memo_mp_length(obj: *mut PyObject) -> Py_ssize_t {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let mut count = (*self_).table.used as Py_ssize_t;
        if !(*self_).keepalive.items.is_empty() {
            count += 1;
        }
        count
    }
}

unsafe extern "C" fn memo_mp_subscript(obj: *mut PyObject, pykey: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;

        if PyLong_Check(pykey) == 0 {
            PyErr_SetObject(PyExc_KeyError, pykey);
            return ptr::null_mut();
        }

        let key = PyLong_AsVoidPtr(pykey) as usize;
        if key == 0 && !PyErr_Occurred().is_null() {
            return ptr::null_mut();
        }

        if key == self_ as usize {
            if (*self_).keepalive.items.is_empty() {
                PyErr_SetObject(PyExc_KeyError, pykey);
                return ptr::null_mut();
            }
            return memo_keepalive_proxy(self_);
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if found.is_null() {
            PyErr_SetObject(PyExc_KeyError, pykey);
            return ptr::null_mut();
        }

        Py_NewRef(found)
    }
}

unsafe extern "C" fn memo_mp_ass_subscript(
    obj: *mut PyObject,
    pykey: *mut PyObject,
    value: *mut PyObject,
) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;

        if PyLong_Check(pykey) == 0 {
            PyErr_SetString(PyExc_KeyError, cstr!("keys must be integers"));
            return -1;
        }

        let key = PyLong_AsVoidPtr(pykey) as usize;
        if key == 0 && !PyErr_Occurred().is_null() {
            return -1;
        }

        if key == self_ as usize {
            if value.is_null() {
                if (*self_).keepalive.items.is_empty() {
                    PyErr_SetObject(PyExc_KeyError, pykey);
                    return -1;
                }
                (*self_).keepalive.clear();
                return 0;
            }

            let it = PyObject_GetIter(value);
            if it.is_null() {
                return -1;
            }

            (*self_).keepalive.clear();

            loop {
                let item = PyIter_Next(it);
                if item.is_null() {
                    break;
                }
                (*self_).keepalive.append(item);
                Py_DECREF(item);
            }

            Py_DECREF(it);

            if !PyErr_Occurred().is_null() {
                return -1;
            }

            return 0;
        }

        if value.is_null() {
            let hash = hash_pointer(key);
            if (*self_).table.remove_h(key, hash) < 0 {
                PyErr_SetObject(PyExc_KeyError, pykey);
                return -1;
            }
            return 0;
        }

        (*self_).insert_logged(key, value, hash_pointer(key))
    }
}

// ── Sequence protocol (sq_contains) ────────────────────────

unsafe extern "C" fn memo_sq_contains(
    obj: *mut PyObject,
    pykey: *mut PyObject,
) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;

        if PyLong_Check(pykey) == 0 {
            PyErr_SetString(PyExc_TypeError, cstr!("keys must be integers"));
            return -1;
        }

        let key = PyLong_AsVoidPtr(pykey) as usize;
        if key == 0 && !PyErr_Occurred().is_null() {
            return -1;
        }

        if key == self_ as usize && !(*self_).keepalive.items.is_empty() {
            return 1;
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if found.is_null() {
            0
        } else {
            1
        }
    }
}

// ── Methods ────────────────────────────────────────────────

unsafe extern "C" fn memo_py_clear(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).table.clear();
        (*self_).keepalive.clear();
        if !(*self_).dict_proxy.is_null() {
            Py_DECREF((*self_).dict_proxy);
            (*self_).dict_proxy = ptr::null_mut();
        }
        Py_NewRef(Py_None())
    }
}

unsafe extern "C" fn memo_py_get(
    obj: *mut PyObject,
    args: *mut *mut PyObject,
    nargs: Py_ssize_t,
) -> *mut PyObject {
    unsafe {
        if nargs < 1 || nargs > 2 {
            PyErr_SetString(PyExc_TypeError, cstr!("get expected 1 or 2 arguments"));
            return ptr::null_mut();
        }

        let self_ = obj as *mut PyMemoObject;
        let pykey = *args;

        if PyLong_Check(pykey) == 0 {
            PyErr_SetString(PyExc_TypeError, cstr!("keys must be integers"));
            return ptr::null_mut();
        }

        let key = PyLong_AsVoidPtr(pykey) as usize;
        if key == 0 && !PyErr_Occurred().is_null() {
            return ptr::null_mut();
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if !found.is_null() {
            return Py_NewRef(found);
        }

        if nargs == 2 {
            return Py_NewRef(*args.add(1));
        }

        Py_NewRef(Py_None())
    }
}

unsafe extern "C" fn memo_py_contains(
    obj: *mut PyObject,
    pykey: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let result = memo_sq_contains(obj, pykey);
        if result < 0 {
            return ptr::null_mut();
        }
        PyBool_FromLong(result as _)
    }
}

unsafe extern "C" fn memo_py_values(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let mut n = (*self_).table.used as Py_ssize_t;
        if !(*self_).keepalive.items.is_empty() {
            n += 1;
        }

        let list = PyList_New(n);
        if list.is_null() {
            return ptr::null_mut();
        }

        let mut idx: Py_ssize_t = 0;
        if !(*self_).table.slots.is_null() {
            for i in 0..(*self_).table.size {
                let entry = &*(*self_).table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    Py_INCREF(entry.value);
                    PyList_SetItem(list, idx, entry.value);
                    idx += 1;
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let proxy = memo_keepalive_proxy(self_);
            if proxy.is_null() {
                Py_DECREF(list);
                return ptr::null_mut();
            }
            PyList_SetItem(list, idx, proxy);
        }

        list
    }
}

unsafe extern "C" fn memo_py_keys(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let mut n = (*self_).table.used as Py_ssize_t;
        if !(*self_).keepalive.items.is_empty() {
            n += 1;
        }

        let list = PyList_New(n);
        if list.is_null() {
            return ptr::null_mut();
        }

        let mut idx: Py_ssize_t = 0;
        if !(*self_).table.slots.is_null() {
            for i in 0..(*self_).table.size {
                let entry = &*(*self_).table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let py_key = PyLong_FromVoidPtr(entry.key as *mut c_void);
                    if py_key.is_null() {
                        Py_DECREF(list);
                        return ptr::null_mut();
                    }
                    PyList_SetItem(list, idx, py_key);
                    idx += 1;
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = PyLong_FromVoidPtr(self_ as *mut c_void);
            if py_key.is_null() {
                Py_DECREF(list);
                return ptr::null_mut();
            }
            PyList_SetItem(list, idx, py_key);
        }

        list
    }
}

unsafe extern "C" fn memo_py_items(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let mut n = (*self_).table.used as Py_ssize_t;
        if !(*self_).keepalive.items.is_empty() {
            n += 1;
        }

        let list = PyList_New(n);
        if list.is_null() {
            return ptr::null_mut();
        }

        let mut idx: Py_ssize_t = 0;
        if !(*self_).table.slots.is_null() {
            for i in 0..(*self_).table.size {
                let entry = &*(*self_).table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let py_key = PyLong_FromVoidPtr(entry.key as *mut c_void);
                    if py_key.is_null() {
                        Py_DECREF(list);
                        return ptr::null_mut();
                    }
                    let pair = PyTuple_New(2);
                    if pair.is_null() {
                        Py_DECREF(py_key);
                        Py_DECREF(list);
                        return ptr::null_mut();
                    }
                    PyTuple_SetItem(pair, 0, py_key);
                    Py_INCREF(entry.value);
                    PyTuple_SetItem(pair, 1, entry.value);
                    PyList_SetItem(list, idx, pair);
                    idx += 1;
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = PyLong_FromVoidPtr(self_ as *mut c_void);
            if py_key.is_null() {
                Py_DECREF(list);
                return ptr::null_mut();
            }
            let proxy = memo_keepalive_proxy(self_);
            if proxy.is_null() {
                Py_DECREF(py_key);
                Py_DECREF(list);
                return ptr::null_mut();
            }
            let pair = PyTuple_New(2);
            if pair.is_null() {
                Py_DECREF(py_key);
                Py_DECREF(proxy);
                Py_DECREF(list);
                return ptr::null_mut();
            }
            PyTuple_SetItem(pair, 0, py_key);
            PyTuple_SetItem(pair, 1, proxy);
            PyList_SetItem(list, idx, pair);
        }

        list
    }
}

unsafe extern "C" fn memo_py_setdefault(
    obj: *mut PyObject,
    args: *mut *mut PyObject,
    nargs: Py_ssize_t,
) -> *mut PyObject {
    unsafe {
        if nargs < 1 || nargs > 2 {
            PyErr_SetString(PyExc_TypeError, cstr!("setdefault expected 1 or 2 arguments"));
            return ptr::null_mut();
        }

        let self_ = obj as *mut PyMemoObject;
        let pykey = *args;

        if PyLong_Check(pykey) == 0 {
            PyErr_SetString(PyExc_KeyError, cstr!("keys must be integers"));
            return ptr::null_mut();
        }

        let key = PyLong_AsVoidPtr(pykey) as usize;
        if key == 0 && !PyErr_Occurred().is_null() {
            return ptr::null_mut();
        }

        if key == self_ as usize {
            return memo_keepalive_proxy(self_);
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if !found.is_null() {
            return Py_NewRef(found);
        }

        let default_value = if nargs == 2 { *args.add(1) } else { Py_None() };
        if (*self_).insert_logged(key, default_value, hash_pointer(key)) < 0 {
            return ptr::null_mut();
        }

        Py_NewRef(default_value)
    }
}

unsafe extern "C" fn memo_py_del(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).table.clear();
        (*self_).keepalive.clear();
        if !(*self_).dict_proxy.is_null() {
            Py_DECREF((*self_).dict_proxy);
            (*self_).dict_proxy = ptr::null_mut();
        }
        Py_NewRef(Py_None())
    }
}

// ── Type initialization ────────────────────────────────────

unsafe fn init_memo_methods() {
    unsafe {
        MEMO_METHODS_TABLE[0] = PyMethodDef {
            ml_name: cstr!("clear"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_clear,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[1] = PyMethodDef {
            ml_name: cstr!("get"),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFast: memo_py_get,
            },
            ml_flags: METH_FASTCALL,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[2] = PyMethodDef {
            ml_name: cstr!("values"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_values,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[3] = PyMethodDef {
            ml_name: cstr!("keys"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_keys,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[4] = PyMethodDef {
            ml_name: cstr!("items"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_items,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[5] = PyMethodDef {
            ml_name: cstr!("setdefault"),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFast: memo_py_setdefault,
            },
            ml_flags: METH_FASTCALL,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[6] = PyMethodDef {
            ml_name: cstr!("__contains__"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_contains,
            },
            ml_flags: METH_O,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[7] = PyMethodDef {
            ml_name: cstr!("__del__"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_del,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[8] = PyMethodDef::zeroed();
    }
}

pub unsafe fn memo_ready_type() -> i32 {
    unsafe {
        init_keepalive_methods();
        if PyType_Ready(ptr::addr_of_mut!(KEEPALIVE_LIST_TYPE)) < 0 {
            return -1;
        }

        init_memo_methods();

        MEMO_MAPPING = PyMappingMethods {
            mp_length: Some(memo_mp_length),
            mp_subscript: Some(memo_mp_subscript),
            mp_ass_subscript: Some(memo_mp_ass_subscript),
        };

        MEMO_SEQUENCE.sq_contains = Some(memo_sq_contains);

        let tp = ptr::addr_of_mut!(Memo_Type);
        (*tp).tp_name = cstr!("copium.memo");
        (*tp).tp_basicsize = std::mem::size_of::<PyMemoObject>() as Py_ssize_t;
        (*tp).tp_dealloc = Some(memo_dealloc);
        (*tp).tp_repr = Some(memo_repr);
        (*tp).tp_as_mapping = ptr::addr_of_mut!(MEMO_MAPPING);
        (*tp).tp_as_sequence = ptr::addr_of_mut!(MEMO_SEQUENCE);
        #[cfg(Py_GIL_DISABLED)]
        {
            (*tp).tp_flags.store(Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, core::sync::atomic::Ordering::Relaxed);
        }
        #[cfg(not(Py_GIL_DISABLED))]
        {
            (*tp).tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
        }
        (*tp).tp_traverse = Some(memo_traverse);
        (*tp).tp_clear = Some(memo_clear_gc);
        (*tp).tp_iter = Some(memo_iter);
        (*tp).tp_methods = MEMO_METHODS_TABLE.as_mut_ptr();
        (*tp).tp_finalize = Some(memo_finalize);

        PyType_Ready(tp)
    }
}
