#![feature(thread_local)]
#![feature(likely_unlikely)]

use core::ffi::{c_void, CStr};
use std::hint::{likely, unlikely};
use std::ptr;

mod py;
mod about;
#[allow(dead_code)]
mod cache;
mod config;
mod copy;
mod deepcopy;
mod dict_iter;
mod extra;
mod fallback;
mod memo;
mod patch;
mod recursion;
mod reduce;
mod state;
use py::*;
use crate::memo::PyMemoObject;
use memo::{AnyMemo, DictMemo};
use state::{MemoMode, STATE};
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
        let mut memo_arg: *mut PyObject = py::NoneObject;

        // ── Fast path: no keyword arguments ─────────────────
        let kwcount = if kwnames.is_null() {
            0
        } else {
            py::tuple::size(kwnames)
        };

        if likely(kwcount == 0) {
            if unlikely(nargs < 1) {
                py::err::set_string(
                    PyExc_TypeError,
                    cstr!("deepcopy() missing 1 required positional argument: 'x'"),
                );
                return ptr::null_mut();
            }
            if unlikely(nargs > 2) {
                py::err::format!(
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
                py::err::format!(
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
                let name = py::tuple::get_item(kwnames, i);
                let val = *args.offset(nargs + i);

                if py::unicode::compare_ascii(name, cstr!("x")) == 0 {
                    if !obj.is_null() {
                        py::err::set_string(
                            PyExc_TypeError,
                            cstr!("deepcopy() got multiple values for argument 'x'"),
                        );
                        return ptr::null_mut();
                    }
                    obj = val;
                } else if py::unicode::compare_ascii(name, cstr!("memo")) == 0 {
                    if seen_memo_kw || nargs == 2 {
                        py::err::set_string(
                            PyExc_TypeError,
                            cstr!("deepcopy() got multiple values for argument 'memo'"),
                        );
                        return ptr::null_mut();
                    }
                    memo_arg = val;
                    seen_memo_kw = true;
                } else {
                    py::err::format!(
                        PyExc_TypeError,
                        cstr!("deepcopy() got an unexpected keyword argument '%U'"),
                        name,
                    );
                    return ptr::null_mut();
                }
            }

            if unlikely(obj.is_null()) {
                py::err::set_string(
                    PyExc_TypeError,
                    cstr!("deepcopy() missing 1 required positional argument: 'x'"),
                );
                return ptr::null_mut();
            }
        }

        // ── Dispatch based on memo type ─────────────────────
        if likely(memo_arg == py::NoneObject) {
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
            let dict = py::dict::new_presized(0);
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
            py::err::set_string(
                PyExc_TypeError,
                cstr!("replace() missing 1 required positional argument: 'obj'"),
            );
            return ptr::null_mut();
        }
        if nargs > 1 {
            py::err::format!(
                PyExc_TypeError,
                cstr!("replace() takes 1 positional argument but %zd were given"),
                nargs,
            );
            return ptr::null_mut();
        }

        let obj = *args;
        let type_pointer = obj.class();
        let class_object = type_pointer as *mut PyObject;
        let func = class_object.getattr_cstr(cstr!("__replace__"));
        if func.is_null() {
            py::err::clear();
            py::err::format!(
                PyExc_TypeError,
                cstr!("replace() does not support %.200s objects"),
                (*type_pointer).tp_name,
            );
            return ptr::null_mut();
        }

        let posargs = py::tuple::new(1);
        if posargs.is_null() {
            func.decref();
            return ptr::null_mut();
        }
        posargs.steal_item_unchecked(0, obj.newref());

        let mut kwargs: *mut PyObject = ptr::null_mut();
        let kwcount = if kwnames.is_null() {
            0
        } else {
            py::tuple::size(kwnames)
        };
        if kwcount > 0 {
            kwargs = py::dict::new().as_object();
            if kwargs.is_null() {
                func.decref();
                posargs.decref();
                return ptr::null_mut();
            }
            for i in 0..kwcount {
                let key = py::tuple::get_item(kwnames, i);
                let val = *args.offset(nargs + i);
                (kwargs as *mut PyDictObject).set_item(key, val);
            }
        }

        let out = func.call_with_kwargs(posargs, kwargs);
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
            ml_name: cstr!("copy").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunction: py_copy,
            },
            ml_flags: METH_O,
            ml_doc: cstr!("copy(obj, /)\n--\n\nReturn a shallow copy of obj.").as_ptr(),
        };
        i += 1;

        MAIN_METHODS[i] = PyMethodDef {
            ml_name: cstr!("deepcopy").as_ptr(),
            ml_meth: PyMethodDefPointer {
                PyCFunctionFastWithKeywords: py_deepcopy,
            },
            ml_flags: METH_FASTCALL | METH_KEYWORDS,
            ml_doc: cstr!("deepcopy(x, memo=None, /)\n--\n\nReturn a deep copy of obj.").as_ptr(),
        };
        i += 1;

        #[cfg(Py_3_13)]
        {
            MAIN_METHODS[i] = PyMethodDef {
                ml_name: cstr!("replace").as_ptr(),
                ml_meth: PyMethodDefPointer {
                    PyCFunctionFastWithKeywords: py_replace,
                },
                ml_flags: METH_FASTCALL | METH_KEYWORDS,
                ml_doc: cstr!("replace(obj, /, **changes)\n--\n\nReplace fields on a copy.")
                    .as_ptr(),
            };
            i += 1;
        }

        MAIN_METHODS[i] = PyMethodDef::zeroed();
    }
}

unsafe extern "C" fn orcopium_exec(module: *mut PyObject) -> i32 {
    unsafe {
        if cache::init() < 0 {
            return -1;
        }

        if state::init() < 0 {
            return -1;
        }

        if dict_iter::dict_iter_module_init() < 0 {
            return -1;
        }

        if memo::memo_ready_type() < 0 {
            return -1;
        }

        if py::module::add_object(module, cstr!("Error"), py_obj!("copy.Error").newref()) < 0 {
            return -1;
        }

        let memo_type = ptr::addr_of_mut!(memo::Memo_Type) as *mut PyObject;
        if py::module::add_object(module, cstr!("memo"), memo_type.newref()) < 0 {
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

        let config_module = module.getattr_cstr(cstr!("config"));
        if config_module.is_null() {
            return -1;
        }

        let configure = config_module.getattr_cstr(cstr!("apply"));
        if configure.is_null() {
            config_module.decref();
            return -1;
        }
        if py::module::add_object(module, cstr!("configure"), configure) < 0 {
            config_module.decref();
            configure.decref();
            return -1;
        }

        let get_config = config_module.getattr_cstr(cstr!("get"));
        config_module.decref();
        if get_config.is_null() {
            return -1;
        }
        if py::module::add_object(module, cstr!("get_config"), get_config) < 0 {
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
        state::cleanup();
    }
}

#[cfg(all(Py_3_14, Py_GIL_DISABLED))]
static mut MODULE_SLOTS: [PyModuleDef_Slot; 3] = [
    PyModuleDef_Slot {
        slot: Py_mod_exec,
        value: orcopium_exec as *mut _,
    },
    PyModuleDef_Slot {
        slot: Py_mod_gil,
        value: Py_MOD_GIL_NOT_USED,
    },
    PyModuleDef_Slot {
        slot: 0,
        value: ptr::null_mut(),
    },
];

#[cfg(not(all(Py_3_14, Py_GIL_DISABLED)))]
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
    m_doc: b"Fast, full-native deepcopy with reduce protocol and keepalive memo.\0"
        .as_ptr()
        .cast(),
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
    name: &CStr,
    submodule: *mut PyObject,
) -> i32 {
    unsafe {
        let canonical = py::unicode::from_format!(cstr!("copium.%s"), name.as_ptr());
        if canonical.is_null() {
            submodule.decref();
            return -1;
        }

        submodule.set_attr_cstr(cstr!("__name__"), canonical);

        let sys_modules = py::module::get_module_dict();
        if !sys_modules.is_null() {
            sys_modules.set_item(canonical, submodule);
        }

        let parent_name = py::module::get_name(parent);
        if !parent_name.is_null() {
            let full_name = py::unicode::from_format!(cstr!("%U.%s"), parent_name, name.as_ptr());
            if !full_name.is_null() {
                if !sys_modules.is_null() {
                    sys_modules.set_item(full_name, submodule);
                }
                full_name.decref();
            }
            parent_name.decref();
        }
        canonical.decref();

        if py::module::add_object(parent, name, submodule) < 0 {
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
        py::module::def_init(std::ptr::addr_of_mut!(MODULE_DEF))
    }
}
