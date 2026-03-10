use core::ffi::c_char;
use pyo3_ffi::*;
use std::ptr;

use crate::ffi_ext;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum MemoMode {
    Native = 0,
    Dict = 1,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OnIncompatible {
    Warn = 0,
    Raise = 1,
    Silent = 2,
}

pub struct ModuleState {
    // interned attribute names
    pub s_reduce_ex: *mut PyObject,
    pub s_reduce: *mut PyObject,
    pub s_deepcopy: *mut PyObject,
    pub s_setstate: *mut PyObject,
    pub s_dict: *mut PyObject,
    pub s_append: *mut PyObject,
    pub s_update: *mut PyObject,
    pub s_new: *mut PyObject,
    pub s_get: *mut PyObject,
    pub s_copy: *mut PyObject,

    pub sentinel: *mut PyObject,

    // cached type pointers (loaded once at init from live runtime)
    pub none_type: *mut PyTypeObject,
    pub not_implemented_type: *mut PyTypeObject,
    pub re_pattern_type: *mut PyTypeObject,
    pub decimal_type: *mut PyTypeObject,
    pub fraction_type: *mut PyTypeObject,

    // stdlib references
    pub copyreg_dispatch: *mut PyDictObject,
    pub copy_error: *mut PyObject,
    pub copyreg_newobj: *mut PyObject,
    pub copyreg_newobj_ex: *mut PyObject,

    // configuration
    pub memo_mode: MemoMode,
    pub on_incompatible: OnIncompatible,
    pub ignored_errors: *mut PyObject,
    pub ignored_errors_joined: *mut PyObject,
}

unsafe impl Sync for ModuleState {}
unsafe impl Send for ModuleState {}

pub static mut STATE: ModuleState = ModuleState {
    s_reduce_ex: ptr::null_mut(),
    s_reduce: ptr::null_mut(),
    s_deepcopy: ptr::null_mut(),
    s_setstate: ptr::null_mut(),
    s_dict: ptr::null_mut(),
    s_append: ptr::null_mut(),
    s_update: ptr::null_mut(),
    s_new: ptr::null_mut(),
    s_get: ptr::null_mut(),
    s_copy: ptr::null_mut(),
    sentinel: ptr::null_mut(),
    none_type: ptr::null_mut(),
    not_implemented_type: ptr::null_mut(),
    re_pattern_type: ptr::null_mut(),
    decimal_type: ptr::null_mut(),
    fraction_type: ptr::null_mut(),
    copyreg_dispatch: ptr::null_mut(),
    copy_error: ptr::null_mut(),
    copyreg_newobj: ptr::null_mut(),
    copyreg_newobj_ex: ptr::null_mut(),
    memo_mode: MemoMode::Native,
    on_incompatible: OnIncompatible::Warn,
    ignored_errors: ptr::null_mut(),
    ignored_errors_joined: ptr::null_mut(),
};

macro_rules! intern {
    ($s:literal) => {
        PyUnicode_InternFromString(cstr!($s))
    };
}

unsafe fn load_type_from(module_name: &str, attr: &str) -> *mut PyTypeObject {
    unsafe {
        let m = PyImport_ImportModule(
            format!("{module_name}\0").as_ptr() as *const c_char,
        );
        if m.is_null() {
            return ptr::null_mut();
        }
        let t = PyObject_GetAttrString(m, format!("{attr}\0").as_ptr() as *const c_char);
        Py_DECREF(m);
        if t.is_null() {
            return ptr::null_mut();
        }
        t as *mut PyTypeObject
    }
}

pub unsafe fn init() -> i32 {
    unsafe {
        let s = std::ptr::addr_of_mut!(STATE);

        // intern strings
        (*s).s_reduce_ex = intern!("__reduce_ex__");
        (*s).s_reduce = intern!("__reduce__");
        (*s).s_deepcopy = intern!("__deepcopy__");
        (*s).s_setstate = intern!("__setstate__");
        (*s).s_dict = intern!("__dict__");
        (*s).s_append = intern!("append");
        (*s).s_update = intern!("update");
        (*s).s_new = intern!("__new__");
        (*s).s_get = intern!("get");
        (*s).s_copy = intern!("__copy__");

        if (*s).s_reduce_ex.is_null()
            || (*s).s_reduce.is_null()
            || (*s).s_deepcopy.is_null()
            || (*s).s_setstate.is_null()
            || (*s).s_dict.is_null()
            || (*s).s_append.is_null()
            || (*s).s_update.is_null()
            || (*s).s_new.is_null()
            || (*s).s_get.is_null()
            || (*s).s_copy.is_null()
        {
            return -1;
        }

        (*s).sentinel = PyList_New(0);
        if (*s).sentinel.is_null() {
            return -1;
        }

        // type pointers from singletons
        (*s).none_type = Py_TYPE(Py_None());
        (*s).not_implemented_type = Py_TYPE(ffi_ext::Py_NotImplemented());

        // stdlib types loaded from modules
        (*s).re_pattern_type = load_type_from("re", "Pattern");
        (*s).decimal_type = load_type_from("decimal", "Decimal");
        (*s).fraction_type = load_type_from("fractions", "Fraction");

        if (*s).re_pattern_type.is_null()
            || (*s).decimal_type.is_null()
            || (*s).fraction_type.is_null()
        {
            return -1;
        }

        // copyreg + copy
        let copyreg = PyImport_ImportModule(cstr!("copyreg"));
        if copyreg.is_null() {
            return -1;
        }
        (*s).copyreg_dispatch = PyObject_GetAttrString(copyreg, cstr!("dispatch_table")) as _;
        (*s).copyreg_newobj = PyObject_GetAttrString(copyreg, cstr!("__newobj__"));
        if (*s).copyreg_newobj.is_null() {
            PyErr_Clear();
            (*s).copyreg_newobj = (*s).sentinel; // placeholder
            Py_INCREF((*s).copyreg_newobj);
        }
        (*s).copyreg_newobj_ex = PyObject_GetAttrString(copyreg, cstr!("__newobj_ex__"));
        if (*s).copyreg_newobj_ex.is_null() {
            PyErr_Clear();
            (*s).copyreg_newobj_ex = (*s).sentinel;
            Py_INCREF((*s).copyreg_newobj_ex);
        }
        Py_DECREF(copyreg);

        let copy_mod = PyImport_ImportModule(cstr!("copy"));
        if copy_mod.is_null() {
            return -1;
        }
        (*s).copy_error = PyObject_GetAttrString(copy_mod, cstr!("Error"));
        Py_DECREF(copy_mod);
        if (*s).copy_error.is_null() {
            return -1;
        }

        // config from env
        load_config_from_env()
    }
}

unsafe fn parse_ignored_errors_from_environment() -> *mut PyObject {
    unsafe {
        let environment_value = match std::env::var("COPIUM_NO_MEMO_FALLBACK_WARNING") {
            Ok(value) if !value.is_empty() => value,
            _ => return PyTuple_New(0),
        };

        let ignored_error_parts: Vec<&str> = environment_value
            .split("::")
            .filter(|part| !part.is_empty())
            .collect();

        let tuple = PyTuple_New(ignored_error_parts.len() as isize);
        if tuple.is_null() {
            return ptr::null_mut();
        }

        for (index, ignored_error_part) in ignored_error_parts.iter().enumerate() {
            let item = PyUnicode_FromStringAndSize(
                ignored_error_part.as_ptr() as *const c_char,
                ignored_error_part.len() as isize,
            );
            if item.is_null() {
                Py_DECREF(tuple);
                return ptr::null_mut();
            }

            if PyTuple_SetItem(tuple, index as isize, item) < 0 {
                Py_DECREF(item);
                Py_DECREF(tuple);
                return ptr::null_mut();
            }
        }

        tuple
    }
}

pub unsafe fn update_suppress_warnings(new_tuple: *mut PyObject) -> i32 {
    unsafe {
        let s = std::ptr::addr_of_mut!(STATE);

        let old_ignored_errors = (*s).ignored_errors;
        (*s).ignored_errors = new_tuple;
        Py_XDECREF(old_ignored_errors);

        Py_XDECREF((*s).ignored_errors_joined);
        (*s).ignored_errors_joined = ptr::null_mut();

        if PyTuple_GET_SIZE(new_tuple) > 0 {
            let separator = PyUnicode_FromString(cstr!("::"));
            if separator.is_null() {
                return -1;
            }

            (*s).ignored_errors_joined = PyUnicode_Join(separator, new_tuple);
            Py_DECREF(separator);
            if (*s).ignored_errors_joined.is_null() {
                return -1;
            }
        }

        0
    }
}

pub unsafe fn load_config_from_env() -> i32 {
    unsafe {
        let s = std::ptr::addr_of_mut!(STATE);
        let use_dict = std::env::var("COPIUM_USE_DICT_MEMO").ok();
        let no_fallback = std::env::var("COPIUM_NO_MEMO_FALLBACK").ok();

        (*s).memo_mode = if use_dict.as_deref().is_some_and(|value| !value.is_empty()) {
            MemoMode::Dict
        } else {
            MemoMode::Native
        };
        (*s).on_incompatible = if no_fallback.as_deref().is_some_and(|value| !value.is_empty()) {
            OnIncompatible::Raise
        } else {
            OnIncompatible::Warn
        };

        let parsed_ignored_errors = parse_ignored_errors_from_environment();
        if parsed_ignored_errors.is_null() {
            return -1;
        }

        update_suppress_warnings(parsed_ignored_errors)
    }
}
