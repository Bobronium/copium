use std::cell::UnsafeCell;
use std::ffi::{CStr, CString};
use std::mem::MaybeUninit;
use std::os::raw::c_int;
use std::ptr;

use crate::py::{self, *};

// ── Slot types ─────────────────────────────────────────────

#[repr(transparent)]
pub struct PtrSlot(UnsafeCell<*mut PyObject>);
unsafe impl Sync for PtrSlot {}

impl PtrSlot {
    pub const fn new() -> Self {
        Self(UnsafeCell::new(ptr::null_mut()))
    }

    #[inline(always)]
    pub unsafe fn get(&self) -> *mut PyObject {
        unsafe { *self.0.get() }
    }

    pub unsafe fn set(&self, val: *mut PyObject) {
        unsafe {
            *self.0.get() = val;
        }
    }
}

#[repr(transparent)]
pub struct Slot<T>(UnsafeCell<MaybeUninit<T>>);
unsafe impl<T> Sync for Slot<T> {}

impl<T: Copy> Slot<T> {
    pub const fn uninit() -> Self {
        Self(UnsafeCell::new(MaybeUninit::uninit()))
    }

    #[inline(always)]
    pub unsafe fn get(&self) -> T {
        unsafe { (*self.0.get()).assume_init() }
    }

    pub unsafe fn set(&self, val: T) {
        unsafe {
            *self.0.get() = MaybeUninit::new(val);
        }
    }
}

// ── Inventory entry types (three-phase init ordering) ──────

macro_rules! init_phase {
    ($($name:ident),+) => {$(
        pub struct $name {
            pub init_fn: unsafe fn() -> c_int,
        }
        unsafe impl Sync for $name {}
        unsafe impl Send for $name {}
        inventory::collect!($name);
    )+};
}

init_phase!(StrEntry, ObjEntry, CacheEntry);

// ── Primitives ─────────────────────────────────────────────

pub unsafe fn intern_str(string: &CStr) -> *mut PyObject {
    unsafe { py::unicode::intern(string).as_object() }
}

/// Resolve a dotted path like "decimal.Decimal" or "xml.etree.ElementTree.Element".
///
/// First segment: try builtins, then import.
/// Subsequent segments: try getattr, then import accumulated dotted path.
pub unsafe fn resolve_path(path: &str) -> *mut PyObject {
    let segments: Vec<&str> = path.split('.').collect();
    if segments.is_empty() {
        unsafe { py::err::set_string(PyExc_ValueError, crate::cstr!("cache::resolve_path: empty path")) };
        return ptr::null_mut();
    }

    let first = match CString::new(segments[0]) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };

    let mut cur: *mut PyObject;
    unsafe {
        let builtins = py::eval::builtins();
        let builtin_hit = if !builtins.is_null() {
            builtins.borrow_item_cstr(first.as_c_str())
        } else {
            ptr::null_mut()
        };

        if !builtin_hit.is_null() {
            cur = builtin_hit.newref();
        } else {
            py::err::clear();
            cur = py::module::import(first.as_c_str());
            if cur.is_null() {
                return ptr::null_mut();
            }
        }
    }

    for i in 1..segments.len() {
        let seg = match CString::new(segments[i]) {
            Ok(s) => s,
            Err(_) => {
                unsafe {
                    cur.decref();
                }
                return ptr::null_mut();
            }
        };

        unsafe {
            let next = cur.getattr_cstr(seg.as_c_str());
            if !next.is_null() {
                cur.decref();
                cur = next;
                continue;
            }
            py::err::clear();

            let dotted: String = segments[..=i].join(".");
            if let Ok(module_path) = CString::new(dotted) {
                let module = py::module::import(module_path.as_c_str());
                if !module.is_null() {
                    cur.decref();
                    cur = module;
                    continue;
                }
            }

            py::err::clear();
            let path_cstr = CString::new(path).unwrap_or_default();
            py::err::format!(
                PyExc_AttributeError,
                crate::cstr!("cache: cannot resolve '%s' in '%s'"),
                seg.as_ptr(),
                path_cstr.as_ptr(),
            );
            cur.decref();
            return ptr::null_mut();
        }
    }

    cur
}

pub unsafe fn resolve_path_optional(path: &str) -> *mut PyObject {
    let result = unsafe { resolve_path(path) };
    if result.is_null() {
        unsafe { py::err::clear() };
    }
    result
}

unsafe fn make_globals() -> *mut PyObject {
    unsafe {
        let globals = py::dict::new();
        if globals.is_null() {
            return ptr::null_mut();
        }
        let builtins = py::eval::builtins();
        if !builtins.is_null()
            && globals.set_item_cstr(crate::cstr!("__builtins__"), builtins) < 0
        {
            globals.decref();
            return ptr::null_mut();
        }
        globals.as_object()
    }
}

pub unsafe fn eval_cstr(code: &CStr) -> *mut PyObject {
    unsafe {
        let globals = make_globals();
        if globals.is_null() {
            return ptr::null_mut();
        }
        let result = py::eval::run_string(code, Py_eval_input, globals, globals);
        globals.decref();
        result
    }
}

pub unsafe fn exec_cstr(code: &CStr) -> *mut PyDictObject {
    unsafe {
        let globals = make_globals();
        if globals.is_null() {
            return ptr::null_mut();
        }
        let result = py::eval::run_string(code, Py_file_input, globals, globals);
        if result.is_null() {
            globals.decref();
            return ptr::null_mut();
        }
        result.decref();
        globals as *mut PyDictObject
    }
}

fn dedent(s: &str) -> CString {
    let min_indent = s
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| l.len() - l.trim_start().len())
        .min()
        .unwrap_or(0);
    let out: String = s
        .lines()
        .map(|l| {
            if l.len() <= min_indent {
                l.trim_start()
            } else {
                &l[min_indent..]
            }
        })
        .collect::<Vec<_>>()
        .join("\n");
    CString::new(out.trim_matches('\n')).unwrap_or_default()
}

pub unsafe fn eval_str(s: &str) -> *mut PyObject {
    let code = dedent(s);
    unsafe { eval_cstr(code.as_c_str()) }
}

pub unsafe fn exec_str(s: &str) -> *mut PyDictObject {
    let code = dedent(s);
    unsafe { exec_cstr(code.as_c_str()) }
}

// ── Init ───────────────────────────────────────────────────

macro_rules! run_phase {
    ($T:ty) => {
        for e in inventory::iter::<$T> {
            if unsafe { (e.init_fn)() } < 0 {
                return -1;
            }
        }
    };
}

pub unsafe fn init() -> c_int {
    run_phase!(StrEntry);
    run_phase!(ObjEntry);
    run_phase!(CacheEntry);
    0
}

// ── Macros ─────────────────────────────────────────────────

#[macro_export]
macro_rules! py_str {
    ($s:literal) => {{
        static SLOT: $crate::cache::PtrSlot = $crate::cache::PtrSlot::new();

        unsafe fn __init() -> ::std::os::raw::c_int {
            let val = $crate::cache::intern_str($crate::cstr!($s));
            if val.is_null() {
                return -1;
            }
            SLOT.set(val);
            0
        }

        ::inventory::submit! { $crate::cache::StrEntry { init_fn: __init } }

        SLOT.get()
    }};
}

#[doc(hidden)]
#[macro_export]
macro_rules! __py_obj_body {
    (required, $path:literal, $SLOT:ident) => {
        unsafe {
            let val = $crate::cache::resolve_path($path);
            if val.is_null() {
                return -1;
            }
            $SLOT.set(val);
            0
        }
    };
    (optional, $path:literal, $SLOT:ident) => {
        unsafe {
            let val = $crate::cache::resolve_path_optional($path);
            $SLOT.set(val);
            0
        }
    };
}

#[doc(hidden)]
#[macro_export]
macro_rules! __py_obj_impl {
    ($mode:ident, $cast:ty, $path:literal) => {{
        static SLOT: $crate::cache::PtrSlot = $crate::cache::PtrSlot::new();

        unsafe fn __init() -> ::std::os::raw::c_int {
            $crate::__py_obj_body!($mode, $path, SLOT)
        }

        ::inventory::submit! { $crate::cache::ObjEntry { init_fn: __init } }

        SLOT.get() as $cast
    }};
}

#[macro_export]
macro_rules! py_obj {
    ($path:literal) => {
        $crate::__py_obj_impl!(required, *mut $crate::py::PyObject, $path)
    };
    (? $path:literal) => {
        $crate::__py_obj_impl!(optional, *mut $crate::py::PyObject, $path)
    };
    ($T:ty, $path:literal) => {
        $crate::__py_obj_impl!(required, *mut $T, $path)
    };
    (? $T:ty, $path:literal) => {
        $crate::__py_obj_impl!(optional, *mut $T, $path)
    };
}

#[macro_export]
macro_rules! py_type {
    ($path:literal) => {{
        static SLOT: $crate::cache::PtrSlot = $crate::cache::PtrSlot::new();

        unsafe fn __init() -> ::std::os::raw::c_int {
            unsafe {
                let val = $crate::cache::resolve_path($path);
                if val.is_null() {
                    return -1;
                }
                if !$crate::py::PyObjectPtr::is_type(val) {
                    $crate::py::err::set_string(
                        $crate::py::PyExc_TypeError,
                        unsafe {
                            ::std::ffi::CStr::from_bytes_with_nul_unchecked(
                                concat!("py_type!(\"", $path, "\"): resolved to non-type\0")
                                    .as_bytes(),
                            )
                        },
                    );
                    $crate::py::PyObjectPtr::decref(val);
                    return -1;
                }
                SLOT.set(val);
                0
            }
        }

        ::inventory::submit! { $crate::cache::ObjEntry { init_fn: __init } }

        unsafe { SLOT.get() as *mut $crate::py::PyTypeObject }
    }};
}

/// Null return from block body = init error (exception must be set).
/// `return` inside the block exits the init closure, not the calling function.
#[macro_export]
macro_rules! py_cache {
    ($($body:tt)+) => {{
        static SLOT: $crate::cache::PtrSlot = $crate::cache::PtrSlot::new();

        #[allow(unused_unsafe)]
        unsafe fn __init() -> ::std::os::raw::c_int {
            let val: *mut $crate::py::PyObject =
                (|| -> *mut $crate::py::PyObject { unsafe { $($body)+ } })();
            if val.is_null() {
                return -1;
            }
            unsafe { SLOT.set(val); }
            0
        }

        ::inventory::submit! { $crate::cache::CacheEntry { init_fn: __init } }

        #[allow(unused_unsafe)]
        SLOT.get()
    }};
}

/// Body returns `Option<T>`, None = init error.
#[macro_export]
macro_rules! py_cache_typed {
    ($T:ty, $($body:tt)+) => {{
        static SLOT: $crate::cache::Slot<$T> = $crate::cache::Slot::uninit();

        #[allow(unused_unsafe)]
        unsafe fn __init() -> ::std::os::raw::c_int {
            let val: Option<$T> =
                (|| -> Option<$T> { unsafe { $($body)+ } })();
            match val {
                Some(v) => {
                    unsafe { SLOT.set(v); }
                    0
                }
                None => -1,
            }
        }

        ::inventory::submit! { $crate::cache::CacheEntry { init_fn: __init } }

        #[allow(unused_unsafe)]
        unsafe { SLOT.get() }
    }};
}

#[macro_export]
macro_rules! py_eval {
    ($($s:literal)+) => {
        unsafe { $crate::cache::eval_str(concat!($($s,)+)) }
    };
}

#[macro_export]
macro_rules! py_exec {
    ($($s:literal)+) => {
        unsafe { $crate::cache::exec_str(concat!($($s,)+)) }
    };
}
