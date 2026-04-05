use std::ptr;

use crate::cstr;
use crate::py::{self, *};

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
    pub sentinel: *mut PyListObject,

    pub memo_mode: MemoMode,
    pub on_incompatible: OnIncompatible,
    pub ignored_errors: *mut PyTupleObject,
    pub ignored_errors_joined: *mut PyUnicodeObject,
}

unsafe impl Sync for ModuleState {}
unsafe impl Send for ModuleState {}

pub static mut STATE: ModuleState = ModuleState {
    sentinel: ptr::null_mut(),
    memo_mode: MemoMode::Native,
    on_incompatible: OnIncompatible::Warn,
    ignored_errors: ptr::null_mut(),
    ignored_errors_joined: ptr::null_mut(),
};

pub unsafe fn init() -> i32 {
    unsafe {
        let s = std::ptr::addr_of_mut!(STATE);

        (*s).sentinel = py::list::new(0);
        if (*s).sentinel.is_null() {
            return -1;
        }

        load_config_from_env()
    }
}

pub unsafe fn cleanup() {}

unsafe fn parse_ignored_errors_from_environment() -> *mut PyTupleObject {
    unsafe {
        let environment_value = match std::env::var("COPIUM_NO_MEMO_FALLBACK_WARNING") {
            Ok(value) if !value.is_empty() => value,
            _ => return py::tuple::new(0),
        };

        let ignored_error_parts: Vec<&str> = environment_value
            .split("::")
            .filter(|part| !part.is_empty())
            .collect();

        let tuple = py::tuple::new(ignored_error_parts.len() as isize);
        if tuple.is_null() {
            return ptr::null_mut();
        }

        for (index, ignored_error_part) in ignored_error_parts.iter().enumerate() {
            let item = py::unicode::from_str_and_size(ignored_error_part);
            if item.is_null() {
                tuple.decref();
                return ptr::null_mut();
            }

            tuple.steal_item_unchecked(index as isize, item);
        }

        tuple
    }
}

pub unsafe fn update_suppress_warnings(new_tuple: *mut PyTupleObject) -> i32 {
    unsafe {
        let s = std::ptr::addr_of_mut!(STATE);

        let old_ignored_errors = (*s).ignored_errors;
        (*s).ignored_errors = new_tuple;
        old_ignored_errors.decref_nullable();

        (*s).ignored_errors_joined.decref_nullable();
        (*s).ignored_errors_joined = ptr::null_mut();

        if new_tuple.length() > 0 {
            let separator = py::unicode::from_cstr(cstr!("::"));
            if separator.is_null() {
                return -1;
            }

            (*s).ignored_errors_joined = separator.join(new_tuple);
            separator.decref();
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
        (*s).on_incompatible = if no_fallback
            .as_deref()
            .is_some_and(|value| !value.is_empty())
        {
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
