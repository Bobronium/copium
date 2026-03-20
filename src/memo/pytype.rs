use pyo3_ffi::*;
use std::ffi::c_void;
use std::ptr;

use super::native::PyMemoObject;
use crate::cstr;
use crate::memo::table::{hash_pointer, TOMBSTONE};
use crate::py;
use crate::types::{PyMapPtr, PyObjectPtr, PyObjectSlotPtr};

#[allow(non_upper_case_globals)]
pub static mut Memo_Type: PyTypeObject = unsafe { std::mem::zeroed() };
static mut MEMO_MAPPING: PyMappingMethods = unsafe { std::mem::zeroed() };
static mut MEMO_SEQUENCE: PySequenceMethods = unsafe { std::mem::zeroed() };
static mut MEMO_METHODS_TABLE: [PyMethodDef; 9] = unsafe { std::mem::zeroed() };

// ══════════════════════════════════════════════════════════════
//  KeepaliveList — proxy type exposing keepalive vec to Python
// ══════════════════════════════════════════════════════════════

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
        let obj = py::gc::new::<PyKeepaliveListObject>(ptr::addr_of_mut!(KEEPALIVE_LIST_TYPE));
        if obj.is_null() {
            return ptr::null_mut();
        }
        (*obj).owner = owner;
        (owner as *mut PyObject).incref();
        py::gc::track(obj as *mut c_void);
        obj as *mut PyObject
    }
}

pub(super) unsafe fn memo_keepalive_proxy(self_: *mut PyMemoObject) -> *mut PyObject {
    unsafe {
        if !(*self_).dict_proxy.is_null() {
            return (*self_).dict_proxy.newref();
        }

        let proxy = keepalive_list_new(self_);
        if proxy.is_null() {
            return ptr::null_mut();
        }

        (*self_).dict_proxy = proxy;
        proxy.newref()
    }
}

unsafe extern "C" fn keepalive_list_dealloc(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        py::gc::untrack(self_ as *mut c_void);
        ((*self_).owner as *mut PyObject).decref_nullable();
        py::gc::delete(self_ as *mut c_void);
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
        (&mut (*self_).owner as *mut *mut PyMemoObject).clear();
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
            py::err::set_string(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }

        let items = &(*(*self_).owner).keepalive.items;
        let n = items.len() as Py_ssize_t;
        let mut i = index;
        if i < 0 {
            i += n;
        }
        if i < 0 || i >= n {
            py::err::set_string(PyExc_IndexError, cstr!("index out of range"));
            return ptr::null_mut();
        }

        items[i as usize].newref()
    }
}

unsafe extern "C" fn keepalive_list_iter(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            py::err::set_string(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }

        let items = &(*(*self_).owner).keepalive.items;
        let n = items.len() as Py_ssize_t;
        let list = py::list::new(n);
        if list.is_null() {
            return ptr::null_mut();
        }

        for (i, &item) in items.iter().enumerate() {
            item.incref();
            py::list::set_item(list, i as Py_ssize_t, item);
        }

        let it = list.get_iter();
        list.decref();
        it
    }
}

unsafe extern "C" fn keepalive_list_repr(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let list = py::seq::to_list(obj).as_object();
        if list.is_null() {
            return ptr::null_mut();
        }
        let inner = list.repr().as_object();
        list.decref();
        if inner.is_null() {
            return ptr::null_mut();
        }
        let wrapped = py::unicode::from_format!(cstr!("keepalive(%U)"), inner).as_object();
        inner.decref();
        wrapped
    }
}

unsafe extern "C" fn keepalive_list_append(
    obj: *mut PyObject,
    arg: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            py::err::set_string(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }
        (*(*self_).owner).keepalive.append(arg);
        py::none().newref()
    }
}

unsafe extern "C" fn keepalive_list_clear_py(
    obj: *mut PyObject,
    _: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyKeepaliveListObject;
        if (*self_).owner.is_null() {
            py::err::set_string(PyExc_SystemError, cstr!("keepalive has no owner"));
            return ptr::null_mut();
        }
        (*(*self_).owner).keepalive.clear();
        py::none().newref()
    }
}

unsafe fn init_keepalive_methods() {
    unsafe {
        KEEPALIVE_LIST_METHODS_TABLE[0] = PyMethodDef {
            ml_name: cstr!("append").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: keepalive_list_append,
            },
            ml_flags: METH_O,
            ml_doc: ptr::null(),
        };
        KEEPALIVE_LIST_METHODS_TABLE[1] = PyMethodDef {
            ml_name: cstr!("clear").as_ptr(),
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
        (*tp).tp_name = cstr!("copium.keepalive").as_ptr();
        (*tp).tp_basicsize = size_of::<PyKeepaliveListObject>() as Py_ssize_t;
        (*tp).tp_dealloc = Some(keepalive_list_dealloc);
        (*tp).tp_repr = Some(keepalive_list_repr);
        (*tp).tp_as_sequence = ptr::addr_of_mut!(KEEPALIVE_LIST_SEQUENCE);
        #[cfg(Py_GIL_DISABLED)]
        {
            (*tp).tp_flags.store(
                Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
                core::sync::atomic::Ordering::Relaxed,
            );
        }
        #[cfg(not(Py_GIL_DISABLED))]
        {
            (*tp).tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
        }
        (*tp).tp_traverse = Some(keepalive_list_traverse);
        (*tp).tp_clear = Some(keepalive_list_clear);
        (*tp).tp_iter = Some(keepalive_list_iter);
        (*tp).tp_methods = ptr::addr_of_mut!(KEEPALIVE_LIST_METHODS_TABLE).cast::<PyMethodDef>();
    }
}

// ══════════════════════════════════════════════════════════════
//  tp_dealloc / tp_finalize / tp_traverse / tp_clear
// ══════════════════════════════════════════════════════════════

unsafe extern "C" fn memo_dealloc(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        py::gc::untrack(self_ as *mut c_void);
        if py::gc::call_finalizer_from_dealloc(obj) != 0 {
            return;
        }
        ptr::drop_in_place(self_);
        py::gc::delete(self_ as *mut c_void);
    }
}

unsafe extern "C" fn memo_finalize(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).table.clear();
        (*self_).keepalive.clear();
        if !(*self_).dict_proxy.is_null() {
            (*self_).dict_proxy.decref();
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
            (*self_).dict_proxy.decref();
            (*self_).dict_proxy = ptr::null_mut();
        }
        0
    }
}

// ══════════════════════════════════════════════════════════════
//  tp_repr / tp_iter
// ══════════════════════════════════════════════════════════════

unsafe extern "C" fn memo_repr(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let dict = (*self_).to_dict();
        if dict.is_null() {
            return ptr::null_mut();
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = py::long::from_ptr(self_ as *mut c_void).as_object();
            if py_key.is_null() {
                dict.decref();
                return ptr::null_mut();
            }

            let proxy = memo_keepalive_proxy(self_);
            if proxy.is_null() {
                py_key.decref();
                dict.decref();
                return ptr::null_mut();
            }

            if (dict as *mut PyDictObject).set_item(py_key, proxy) < 0 {
                proxy.decref();
                py_key.decref();
                dict.decref();
                return ptr::null_mut();
            }

            proxy.decref();
            py_key.decref();
        }

        let inner = dict.repr().as_object();
        dict.decref();
        if inner.is_null() {
            return ptr::null_mut();
        }

        let wrapped = py::unicode::from_format!(cstr!("memo(%U)"), inner).as_object();
        inner.decref();
        wrapped
    }
}

unsafe extern "C" fn memo_iter(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let table = &(*self_).table;

        let list = py::list::new(0).as_object();
        if list.is_null() {
            return ptr::null_mut();
        }

        if !table.slots.is_null() {
            for i in 0..table.size {
                let entry = &*table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let py_key = py::long::from_ptr(entry.key as *mut c_void).as_object();
                    if py_key.is_null() || py::list::append(list, py_key) < 0 {
                        py_key.decref_nullable();
                        list.decref();
                        return ptr::null_mut();
                    }
                    py_key.decref();
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = py::long::from_ptr(self_ as *mut c_void).as_object();
            if py_key.is_null() || py::list::append(list, py_key) < 0 {
                py_key.decref_nullable();
                list.decref();
                return ptr::null_mut();
            }
            py_key.decref();
        }

        let it = list.get_iter();
        list.decref();
        it
    }
}

// ══════════════════════════════════════════════════════════════
//  Mapping protocol
// ══════════════════════════════════════════════════════════════

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

        if !py::long::check(pykey) {
            py::err::set(PyExc_KeyError, pykey);
            return ptr::null_mut();
        }

        let key = py::long::as_ptr(pykey) as usize;
        if key == 0 && !py::err::occurred().is_null() {
            return ptr::null_mut();
        }

        if key == self_ as usize {
            if (*self_).keepalive.items.is_empty() {
                py::err::set(PyExc_KeyError, pykey);
                return ptr::null_mut();
            }
            return memo_keepalive_proxy(self_);
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if found.is_null() {
            py::err::set(PyExc_KeyError, pykey);
            return ptr::null_mut();
        }

        found.newref()
    }
}

unsafe extern "C" fn memo_mp_ass_subscript(
    obj: *mut PyObject,
    pykey: *mut PyObject,
    value: *mut PyObject,
) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;

        if !py::long::check(pykey) {
            py::err::set_string(PyExc_KeyError, cstr!("keys must be integers"));
            return -1;
        }

        let key = py::long::as_ptr(pykey) as usize;
        if key == 0 && !py::err::occurred().is_null() {
            return -1;
        }

        if key == self_ as usize {
            if value.is_null() {
                if (*self_).keepalive.items.is_empty() {
                    py::err::set(PyExc_KeyError, pykey);
                    return -1;
                }
                (*self_).keepalive.clear();
                return 0;
            }

            let it = value.get_iter();
            if it.is_null() {
                return -1;
            }

            (*self_).keepalive.clear();

            loop {
                let item = it.iter_next();
                if item.is_null() {
                    break;
                }
                (*self_).keepalive.append(item);
                item.decref();
            }

            it.decref();

            if !py::err::occurred().is_null() {
                return -1;
            }

            return 0;
        }

        if value.is_null() {
            let hash = hash_pointer(key);
            if (*self_).table.remove_h(key, hash) < 0 {
                py::err::set(PyExc_KeyError, pykey);
                return -1;
            }
            return 0;
        }

        (*self_).insert_logged(key, value, hash_pointer(key))
    }
}

// ══════════════════════════════════════════════════════════════
//  Sequence protocol (sq_contains)
// ══════════════════════════════════════════════════════════════

unsafe extern "C" fn memo_sq_contains(obj: *mut PyObject, pykey: *mut PyObject) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;

        if !py::long::check(pykey) {
            py::err::set_string(PyExc_TypeError, cstr!("keys must be integers"));
            return -1;
        }

        let key = py::long::as_ptr(pykey) as usize;
        if key == 0 && !py::err::occurred().is_null() {
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

// ══════════════════════════════════════════════════════════════
//  Methods
// ══════════════════════════════════════════════════════════════

unsafe extern "C" fn memo_py_clear(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).table.clear();
        (*self_).keepalive.clear();
        if !(*self_).dict_proxy.is_null() {
            (*self_).dict_proxy.decref();
            (*self_).dict_proxy = ptr::null_mut();
        }
        py::none().newref()
    }
}

unsafe extern "C" fn memo_py_get(
    obj: *mut PyObject,
    args: *mut *mut PyObject,
    nargs: Py_ssize_t,
) -> *mut PyObject {
    unsafe {
        if nargs < 1 || nargs > 2 {
            py::err::set_string(PyExc_TypeError, cstr!("get expected 1 or 2 arguments"));
            return ptr::null_mut();
        }

        let self_ = obj as *mut PyMemoObject;
        let pykey = *args;

        if !py::long::check(pykey) {
            py::err::set_string(PyExc_TypeError, cstr!("keys must be integers"));
            return ptr::null_mut();
        }

        let key = py::long::as_ptr(pykey) as usize;
        if key == 0 && !py::err::occurred().is_null() {
            return ptr::null_mut();
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if !found.is_null() {
            return found.newref();
        }

        if nargs == 2 {
            return (*args.add(1)).newref();
        }

        py::none().newref()
    }
}

unsafe extern "C" fn memo_py_contains(obj: *mut PyObject, pykey: *mut PyObject) -> *mut PyObject {
    unsafe {
        let result = memo_sq_contains(obj, pykey);
        if result < 0 {
            return ptr::null_mut();
        }
        py::boolean::from_bool(result != 0)
    }
}

unsafe extern "C" fn memo_py_values(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let mut n = (*self_).table.used as Py_ssize_t;
        if !(*self_).keepalive.items.is_empty() {
            n += 1;
        }

        let list = py::list::new(n).as_object();
        if list.is_null() {
            return ptr::null_mut();
        }

        let mut idx: Py_ssize_t = 0;
        if !(*self_).table.slots.is_null() {
            for i in 0..(*self_).table.size {
                let entry = &*(*self_).table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    entry.value.incref();
                    py::list::set_item(list, idx, entry.value);
                    idx += 1;
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let proxy = memo_keepalive_proxy(self_);
            if proxy.is_null() {
                list.decref();
                return ptr::null_mut();
            }
            py::list::set_item(list, idx, proxy);
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

        let list = py::list::new(n).as_object();
        if list.is_null() {
            return ptr::null_mut();
        }

        let mut idx: Py_ssize_t = 0;
        if !(*self_).table.slots.is_null() {
            for i in 0..(*self_).table.size {
                let entry = &*(*self_).table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let py_key = py::long::from_ptr(entry.key as *mut c_void).as_object();
                    if py_key.is_null() {
                        list.decref();
                        return ptr::null_mut();
                    }
                    py::list::set_item(list, idx, py_key);
                    idx += 1;
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = py::long::from_ptr(self_ as *mut c_void).as_object();
            if py_key.is_null() {
                list.decref();
                return ptr::null_mut();
            }
            py::list::set_item(list, idx, py_key);
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

        let list = py::list::new(n).as_object();
        if list.is_null() {
            return ptr::null_mut();
        }

        let mut idx: Py_ssize_t = 0;
        if !(*self_).table.slots.is_null() {
            for i in 0..(*self_).table.size {
                let entry = &*(*self_).table.slots.add(i);
                if entry.key != 0 && entry.key != TOMBSTONE {
                    let py_key = py::long::from_ptr(entry.key as *mut c_void).as_object();
                    if py_key.is_null() {
                        list.decref();
                        return ptr::null_mut();
                    }
                    let pair = py::tuple::new(2).as_object();
                    if pair.is_null() {
                        py_key.decref();
                        list.decref();
                        return ptr::null_mut();
                    }
                    py::tuple::set_item(pair, 0, py_key);
                    entry.value.incref();
                    py::tuple::set_item(pair, 1, entry.value);
                    py::list::set_item(list, idx, pair);
                    idx += 1;
                }
            }
        }

        if !(*self_).keepalive.items.is_empty() {
            let py_key = py::long::from_ptr(self_ as *mut c_void).as_object();
            if py_key.is_null() {
                list.decref();
                return ptr::null_mut();
            }
            let proxy = memo_keepalive_proxy(self_);
            if proxy.is_null() {
                py_key.decref();
                list.decref();
                return ptr::null_mut();
            }
            let pair = py::tuple::new(2).as_object();
            if pair.is_null() {
                py_key.decref();
                proxy.decref();
                list.decref();
                return ptr::null_mut();
            }
            py::tuple::set_item(pair, 0, py_key);
            py::tuple::set_item(pair, 1, proxy);
            py::list::set_item(list, idx, pair);
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
            py::err::set_string(
                PyExc_TypeError,
                cstr!("setdefault expected 1 or 2 arguments"),
            );
            return ptr::null_mut();
        }

        let self_ = obj as *mut PyMemoObject;
        let pykey = *args;

        if !py::long::check(pykey) {
            py::err::set_string(PyExc_KeyError, cstr!("keys must be integers"));
            return ptr::null_mut();
        }

        let key = py::long::as_ptr(pykey) as usize;
        if key == 0 && !py::err::occurred().is_null() {
            return ptr::null_mut();
        }

        if key == self_ as usize {
            return memo_keepalive_proxy(self_);
        }

        let found = (*self_).table.lookup_h(key, hash_pointer(key));
        if !found.is_null() {
            return found.newref();
        }

        let default_value = if nargs == 2 { *args.add(1) } else { py::none() };
        if (*self_).insert_logged(key, default_value, hash_pointer(key)) < 0 {
            return ptr::null_mut();
        }

        default_value.newref()
    }
}

// ══════════════════════════════════════════════════════════════
//  Type initialization
// ══════════════════════════════════════════════════════════════

unsafe fn init_memo_methods() {
    unsafe {
        MEMO_METHODS_TABLE[0] = PyMethodDef {
            ml_name: cstr!("clear").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_clear,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[1] = PyMethodDef {
            ml_name: cstr!("get").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFast: memo_py_get,
            },
            ml_flags: METH_FASTCALL,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[2] = PyMethodDef {
            ml_name: cstr!("values").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_values,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[3] = PyMethodDef {
            ml_name: cstr!("keys").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_keys,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[4] = PyMethodDef {
            ml_name: cstr!("items").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_items,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[5] = PyMethodDef {
            ml_name: cstr!("setdefault").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFast: memo_py_setdefault,
            },
            ml_flags: METH_FASTCALL,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[6] = PyMethodDef {
            ml_name: cstr!("__contains__").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_contains,
            },
            ml_flags: METH_O,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[7] = PyMethodDef {
            ml_name: cstr!("__del__").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_clear,
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
        if py::type_object::ready(ptr::addr_of_mut!(KEEPALIVE_LIST_TYPE)) < 0 {
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
        (*tp).tp_name = cstr!("copium.memo").as_ptr();
        (*tp).tp_basicsize = std::mem::size_of::<PyMemoObject>() as Py_ssize_t;
        (*tp).tp_dealloc = Some(memo_dealloc);
        (*tp).tp_repr = Some(memo_repr);
        (*tp).tp_as_mapping = ptr::addr_of_mut!(MEMO_MAPPING);
        (*tp).tp_as_sequence = ptr::addr_of_mut!(MEMO_SEQUENCE);
        #[cfg(Py_GIL_DISABLED)]
        {
            (*tp).tp_flags.store(
                Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
                core::sync::atomic::Ordering::Relaxed,
            );
        }
        #[cfg(not(Py_GIL_DISABLED))]
        {
            (*tp).tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
        }
        (*tp).tp_traverse = Some(memo_traverse);
        (*tp).tp_clear = Some(memo_clear_gc);
        (*tp).tp_iter = Some(memo_iter);
        (*tp).tp_methods = ptr::addr_of_mut!(MEMO_METHODS_TABLE).cast::<PyMethodDef>();
        (*tp).tp_finalize = Some(memo_finalize);

        py::type_object::ready(tp)
    }
}
