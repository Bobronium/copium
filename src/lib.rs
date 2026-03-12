#![feature(thread_local)]
#![feature(likely_unlikely)]

use core::ffi::{c_char, c_void};
use pyo3_ffi::*;
use std::ptr;
use std::hint::{likely, unlikely};

#[macro_use]
mod ffi_ext;
mod state;
mod types;
mod recursion;
mod memo;
mod dict_iter;
mod reduce;
mod fallback;
mod deepcopy;
mod copy;
mod config;
mod about;
mod extra;
mod patch;
mod compat;
mod critical_section;

use memo::{AnyMemo, DictMemo};
use state::{MemoMode, STATE};
use crate::memo::PyMemoObject;
use crate::types::{py_dict_new, PyObjectPtr, PyTypeObjectPtr, PyTypeInfo};
// ══════════════════════════════════════════════════════════════
//  copy(obj, /) — METH_O
// ══════════════════════════════════════════════════════════════

unsafe extern "C" fn py_copy(_self: *mut PyObject, obj: *mut PyObject) -> *mut PyObject {
    unsafe { copy::copy(obj).into_raw() }
}

// ══════════════════════════════════════════════════════════════
//  deepcopy(x, memo=None, /) — METH_FASTCALL | METH_KEYWORDS
// ══════════════════════════════════════════════════════════════

pub(crate) unsafe extern "C" fn py_deepcopy(
    _self: *mut PyObject,
    args: *const *mut PyObject,
    nargs: Py_ssize_t,
    kwnames: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        let mut obj: *mut PyObject = ptr::null_mut();
        let mut memo_arg: *mut PyObject = Py_None();

        // ── Fast path: no keyword arguments ─────────────────
        let kwcount = if kwnames.is_null() {
            0
        } else {
            PyTuple_Size(kwnames)
        };

        if likely(kwcount == 0) {
            if unlikely(nargs < 1) {
                PyErr_SetString(
                    PyExc_TypeError,
                    cstr!("deepcopy() missing 1 required positional argument: 'x'"),
                );
                return ptr::null_mut();
            }
            if unlikely(nargs > 2) {
                PyErr_Format(
                    PyExc_TypeError,
                    cstr!("deepcopy() takes from 1 to 2 positional arguments but %zd were given"),
                    nargs,
                );
                return ptr::null_mut();
            }
            obj = *args;
            if nargs == 2 {
                memo_arg = *args.add(1);
            }
        } else {
            // ── Keyword argument handling ────────────────────
            if unlikely(nargs > 2) {
                PyErr_Format(
                    PyExc_TypeError,
                    cstr!("deepcopy() takes from 1 to 2 positional arguments but %zd were given"),
                    nargs,
                );
                return ptr::null_mut();
            }

            if nargs >= 1 {
                obj = *args;
            }
            if nargs == 2 {
                memo_arg = *args.add(1);
            }

            let mut seen_memo_kw = false;
            for i in 0..kwcount {
                let name = PyTuple_GetItem(kwnames, i);
                let val = *args.offset(nargs + i);

                if PyUnicode_CompareWithASCIIString(name, cstr!("x")) == 0 {
                    if !obj.is_null() {
                        PyErr_SetString(
                            PyExc_TypeError,
                            cstr!("deepcopy() got multiple values for argument 'x'"),
                        );
                        return ptr::null_mut();
                    }
                    obj = val;
                } else if PyUnicode_CompareWithASCIIString(name, cstr!("memo")) == 0 {
                    if seen_memo_kw || nargs == 2 {
                        PyErr_SetString(
                            PyExc_TypeError,
                            cstr!("deepcopy() got multiple values for argument 'memo'"),
                        );
                        return ptr::null_mut();
                    }
                    memo_arg = val;
                    seen_memo_kw = true;
                } else {
                    PyErr_Format(
                        PyExc_TypeError,
                        cstr!("deepcopy() got an unexpected keyword argument '%U'"),
                        name,
                    );
                    return ptr::null_mut();
                }
            }

            if unlikely(obj.is_null()) {
                PyErr_SetString(
                    PyExc_TypeError,
                    cstr!("deepcopy() missing 1 required positional argument: 'x'"),
                );
                return ptr::null_mut();
            }
        }

        // ── Dispatch based on memo type ─────────────────────
        if likely(memo_arg == Py_None()) {
            let tp = obj.class();
            if tp.is_atomic_immutable() {
                return obj.newref();
            }

            if likely(STATE.memo_mode == MemoMode::Native) {
                let (pm, is_tss) = memo::get_memo();
                if unlikely(pm.is_null()) {
                    return ptr::null_mut();
                }
                let result = deepcopy::deepcopy(obj, &mut *pm);
                memo::cleanup_memo(pm, is_tss);
                return result.into_raw();
            }

            // memo="dict" config
            let dict = py_dict_new(0);
            if dict.is_null() {
                return ptr::null_mut();
            }
            let mut m = DictMemo::new(dict as _);
            let result = deepcopy::deepcopy(obj, &mut m);
            drop(m);
            dict.decref();
            return result.into_raw();
        }

        let memo_type = memo_arg.class();

        if let Some(memo) = PyMemoObject::cast_exact(memo_arg, memo_type) {
            let result = deepcopy::deepcopy(obj, &mut *memo);
            return result.into_raw();
        }

        if let Some(memo) = PyDictObject::cast_exact(memo_arg, memo_type) {
            let mut m = DictMemo::new(memo);
            let result = deepcopy::deepcopy(obj, &mut m);
            return result.into_raw();
        }

        // Any other mapping-like object
        let mut m = AnyMemo::new(memo_arg);
        let result = deepcopy::deepcopy(obj, &mut m);
        result.into_raw()
    }
}

// ══════════════════════════════════════════════════════════════
//  replace(obj, /, **changes) — 3.13+ only
// ══════════════════════════════════════════════════════════════

#[cfg(Py_3_13)]
unsafe extern "C" fn py_replace(
    _self: *mut PyObject,
    args: *const *mut PyObject,
    nargs: Py_ssize_t,
    kwnames: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        if nargs == 0 {
            PyErr_SetString(
                PyExc_TypeError,
                cstr!("replace() missing 1 required positional argument: 'obj'"),
            );
            return ptr::null_mut();
        }
        if nargs > 1 {
            PyErr_Format(
                PyExc_TypeError,
                cstr!("replace() takes 1 positional argument but %zd were given"),
                nargs,
            );
            return ptr::null_mut();
        }

        let obj = *args;
        let type_pointer = obj.class();
        let class_object = type_pointer as *mut PyObject;
        let func = PyObject_GetAttrString(class_object, cstr!("__replace__"));
        if func.is_null() {
            PyErr_Clear();
            PyErr_Format(
                PyExc_TypeError,
                cstr!("replace() does not support %.200s objects"),
                (*type_pointer).tp_name,
            );
            return ptr::null_mut();
        }

        let posargs = PyTuple_New(1);
        if posargs.is_null() {
            func.decref();
            return ptr::null_mut();
        }
        PyTuple_SetItem(posargs, 0, obj.newref());

        let mut kwargs: *mut PyObject = ptr::null_mut();
        let kwcount = if kwnames.is_null() {
            0
        } else {
            PyTuple_Size(kwnames)
        };
        if kwcount > 0 {
            kwargs = PyDict_New();
            if kwargs.is_null() {
                func.decref();
                posargs.decref();
                return ptr::null_mut();
            }
            for i in 0..kwcount {
                let key = PyTuple_GetItem(kwnames, i);
                let val = *args.offset(nargs + i);
                PyDict_SetItem(kwargs, key, val);
            }
        }

        let out = PyObject_Call(func, posargs, kwargs);
        func.decref();
        posargs.decref();
        kwargs.decref_nullable();
        out
    }
}

// ══════════════════════════════════════════════════════════════
//  Module definition
// ══════════════════════════════════════════════════════════════

static mut MAIN_METHODS: [PyMethodDef; 4] = [PyMethodDef::zeroed(); 4];

unsafe fn init_methods() {
    unsafe {
        let mut i = 0usize;

        MAIN_METHODS[i] = PyMethodDef {
            ml_name: cstr!("copy"),
            ml_meth: PyMethodDefPointer {
                PyCFunction: py_copy,
            },
            ml_flags: METH_O,
            ml_doc: cstr!("copy(obj, /)\n--\n\nReturn a shallow copy of obj."),
        };
        i += 1;

        MAIN_METHODS[i] = PyMethodDef {
            ml_name: cstr!("deepcopy"),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFastWithKeywords: py_deepcopy,
            },
            ml_flags: METH_FASTCALL | METH_KEYWORDS,
            ml_doc: cstr!("deepcopy(x, memo=None, /)\n--\n\nReturn a deep copy of obj."),
        };
        i += 1;

        #[cfg(Py_3_13)]
        {
            MAIN_METHODS[i] = PyMethodDef {
                ml_name: cstr!("replace"),
                ml_meth: PyMethodDefPointer {
                    PyCFunctionFastWithKeywords: py_replace,
                },
                ml_flags: METH_FASTCALL | METH_KEYWORDS,
                ml_doc: cstr!("replace(obj, /, **changes)\n--\n\nReplace fields on a copy."),
            };
            i += 1;
        }

        MAIN_METHODS[i] = PyMethodDef::zeroed();
    }
}

unsafe extern "C" fn orcopium_exec(module: *mut PyObject) -> i32 {
    unsafe {
        if state::init() < 0 {
            return -1;
        }

        if dict_iter::dict_iter_module_init() < 0 {
            return -1;
        }

        if memo::memo_ready_type() < 0 {
            return -1;
        }

        if PyModule_AddObject(module, cstr!("Error"), STATE.copy_error.newref()) < 0 {
            return -1;
        }

        let memo_type = ptr::addr_of_mut!(memo::Memo_Type) as *mut PyObject;
        if PyModule_AddObject(module, cstr!("memo"), memo_type.newref()) < 0 {
            return -1;
        }

        if extra::create_module(module) < 0 {
            return -1;
        }
        if patch::create_module(module) < 0 {
            return -1;
        }
        if config::create_module(module) < 0 {
            return -1;
        }

        let config_module = PyObject_GetAttrString(module, cstr!("config"));
        if config_module.is_null() {
            return -1;
        }

        let configure = PyObject_GetAttrString(config_module, cstr!("apply"));
        if configure.is_null() {
            config_module.decref();
            return -1;
        }
        if PyModule_AddObject(module, cstr!("configure"), configure) < 0 {
            config_module.decref();
            configure.decref();
            return -1;
        }

        let get_config = PyObject_GetAttrString(config_module, cstr!("get"));
        config_module.decref();
        if get_config.is_null() {
            return -1;
        }
        if PyModule_AddObject(module, cstr!("get_config"), get_config) < 0 {
            get_config.decref();
            return -1;
        }

        if about::create_module(module) < 0 {
            return -1;
        }

        0
    }
}

unsafe extern "C" fn orcopium_free(_: *mut c_void) {
    unsafe {
        dict_iter::dict_iter_module_cleanup();
    }
}

static mut MODULE_SLOTS: [PyModuleDef_Slot; 2] = [
    PyModuleDef_Slot {
        slot: Py_mod_exec,
        value: orcopium_exec as *mut _,
    },
    PyModuleDef_Slot {
        slot: 0,
        value: ptr::null_mut(),
    },
];

static mut MODULE_DEF: PyModuleDef = PyModuleDef {
    m_base: PyModuleDef_HEAD_INIT,
    m_name: b"copium\0".as_ptr().cast(),
    m_doc: b"Fast, full-native deepcopy with reduce protocol and keepalive memo.\0".as_ptr().cast(),
    m_size: 0,
    m_methods: ptr::null_mut(),
    m_slots: ptr::null_mut(),
    m_traverse: None,
    m_clear: None,
    m_free: Some(orcopium_free),
};

/// Register a submodule on the parent and in sys.modules.
pub unsafe fn add_submodule(
    parent: *mut PyObject,
    name: *const c_char,
    submodule: *mut PyObject,
) -> i32 {
    unsafe {
        // Always register as "copium.<name>" regardless of internal module nesting
        let canonical = PyUnicode_FromFormat(cstr!("copium.%s"), name);
        if canonical.is_null() {
            submodule.decref();
            return -1;
        }

        PyObject_SetAttrString(submodule, cstr!("__name__"), canonical);

        let sys_modules = PyImport_GetModuleDict();
        if !sys_modules.is_null() {
            PyDict_SetItem(sys_modules, canonical, submodule);
        }

        // Also register under the actual parent name (e.g. orcopium.orcopium.extra)
        let parent_name = PyModule_GetNameObject(parent);
        if !parent_name.is_null() {
            let full_name = PyUnicode_FromFormat(cstr!("%U.%s"), parent_name, name);
            if !full_name.is_null() {
                if !sys_modules.is_null() {
                    PyDict_SetItem(sys_modules, full_name, submodule);
                }
                full_name.decref();
            }
            parent_name.decref();
        }
        canonical.decref();

        if PyModule_AddObject(parent, name, submodule) < 0 {
            submodule.decref();
            return -1;
        }

        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn PyInit_copium() -> *mut PyObject {
    unsafe {
        init_methods();
        MODULE_DEF.m_methods = ptr::addr_of_mut!(MAIN_METHODS).cast::<PyMethodDef>();
        MODULE_DEF.m_slots = ptr::addr_of_mut!(MODULE_SLOTS).cast::<PyModuleDef_Slot>();
        PyModuleDef_Init(std::ptr::addr_of_mut!(MODULE_DEF))
    }
}
