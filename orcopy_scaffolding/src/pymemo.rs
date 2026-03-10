use pyo3_ffi::*;
use std::ffi::c_void;
use std::ptr;

use crate::memo::{hash_pointer, Memo, MemoCheckpoint, PyMemoObject, TOMBSTONE};
use crate::types::*;

extern "C" {
    fn _PyObject_GC_New(tp: *mut PyTypeObject) -> *mut PyObject;
}

#[repr(C)]
pub struct PyMemoObject {
    pub ob_base: PyObject,
    pub inner: PyMemoObject,
}

pub static mut Memo_Type: PyTypeObject = unsafe { std::mem::zeroed() };
static mut MEMO_MAPPING: PyMappingMethods = unsafe { std::mem::zeroed() };
static mut MEMO_SEQUENCE: PySequenceMethods = unsafe { std::mem::zeroed() };
static mut MEMO_METHODS_TABLE: [PyMethodDef; 5] = unsafe { std::mem::zeroed() };

unsafe impl PyTypeInfo for PyMemoObject {
    fn type_ptr() -> *mut PyTypeObject {
        unsafe { ptr::addr_of_mut!(Memo_Type) }
    }
}

// ── TSS ────────────────────────────────────────────────────

#[thread_local]
static mut TSS_MEMO: *mut PyMemoObject = ptr::null_mut();

unsafe fn pymemo_alloc() -> *mut PyMemoObject {
    unsafe {
        let obj = _PyObject_GC_New(ptr::addr_of_mut!(Memo_Type));
        if obj.is_null() {
            return ptr::null_mut();
        }
        let memo = obj as *mut PyMemoObject;
        ptr::write(&mut (*memo).inner, PyMemoObject::new());
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
            (fresh, true)
        } else if tss.refcount() == 1 {
            (tss, true)
        } else {
            (pymemo_alloc(), false)
        }
    }
}

pub unsafe fn cleanup_memo(memo: *mut PyMemoObject, is_tss: bool) {
    unsafe {
        if is_tss && Py_REFCNT(memo as *mut PyObject) == 1 {
            (*memo).inner.reset();
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

// ── tp_dealloc / tp_finalize / tp_traverse / tp_clear ──────

unsafe extern "C" fn memo_dealloc(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        PyObject_GC_UnTrack(self_ as *mut c_void);
        if PyObject_CallFinalizerFromDealloc(obj) != 0 {
            return;
        }
        ptr::drop_in_place(&mut (*self_).inner);
        PyObject_GC_Del(self_ as *mut c_void);
    }
}

unsafe extern "C" fn memo_finalize(obj: *mut PyObject) {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).inner.table.clear();
        (*self_).inner.keepalive.clear();
    }
}

unsafe extern "C" fn memo_traverse(
    obj: *mut PyObject,
    visit: visitproc,
    arg: *mut c_void,
) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let inner = &(*self_).inner;

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

        0
    }
}

unsafe extern "C" fn memo_clear_gc(obj: *mut PyObject) -> std::ffi::c_int {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).inner.table.clear();
        (*self_).inner.keepalive.clear();
        0
    }
}

// ── tp_repr / tp_iter ──────────────────────────────────────

unsafe extern "C" fn memo_repr(_obj: *mut PyObject) -> *mut PyObject {
    unsafe { PyUnicode_FromString(cstr!("memo(...)")) }
}

unsafe extern "C" fn memo_iter(obj: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        let table = &(*self_).inner.table;

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

        if !(*self_).inner.keepalive.items.is_empty() {
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
        let mut count = (*self_).inner.table.used as Py_ssize_t;
        if !(*self_).inner.keepalive.items.is_empty() {
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

        if key == self_ as usize && !(*self_).inner.keepalive.items.is_empty() {
            let kv = &(*self_).inner.keepalive.items;
            let n = kv.len() as Py_ssize_t;
            let list = PyList_New(n);
            if list.is_null() {
                return ptr::null_mut();
            }
            for (i, &item) in kv.iter().enumerate() {
                Py_INCREF(item);
                PyList_SetItem(list, i as Py_ssize_t, item);
            }
            return list;
        }

        let found = (*self_).inner.table.lookup_h(key, hash_pointer(key));
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

        if value.is_null() {
            let hash = hash_pointer(key);
            if (*self_).inner.table.remove_h(key, hash) < 0 {
                PyErr_SetObject(PyExc_KeyError, pykey);
                return -1;
            }
            return 0;
        }

        (*self_).inner.insert_logged(key, value, hash_pointer(key))
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

        if key == self_ as usize && !(*self_).inner.keepalive.items.is_empty() {
            return 1;
        }

        let found = (*self_).inner.table.lookup_h(key, hash_pointer(key));
        if found.is_null() { 0 } else { 1 }
    }
}

// ── Methods ────────────────────────────────────────────────

unsafe extern "C" fn memo_py_clear(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).inner.table.clear();
        (*self_).inner.keepalive.clear();
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

        let found = (*self_).inner.table.lookup_h(key, hash_pointer(key));
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

unsafe extern "C" fn memo_py_del(obj: *mut PyObject, _: *mut PyObject) -> *mut PyObject {
    unsafe {
        let self_ = obj as *mut PyMemoObject;
        (*self_).inner.table.clear();
        (*self_).inner.keepalive.clear();
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
            ml_name: cstr!("__contains__"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_contains,
            },
            ml_flags: METH_O,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[3] = PyMethodDef {
            ml_name: cstr!("__del__"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: memo_py_del,
            },
            ml_flags: METH_NOARGS,
            ml_doc: ptr::null(),
        };
        MEMO_METHODS_TABLE[4] = PyMethodDef::zeroed();
    }
}

pub unsafe fn memo_ready_type() -> i32 {
    unsafe {
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
        (*tp).tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC;
        (*tp).tp_traverse = Some(memo_traverse);
        (*tp).tp_clear = Some(memo_clear_gc);
        (*tp).tp_iter = Some(memo_iter);
        (*tp).tp_methods = MEMO_METHODS_TABLE.as_mut_ptr();
        (*tp).tp_finalize = Some(memo_finalize);

        PyType_Ready(tp)
    }
}