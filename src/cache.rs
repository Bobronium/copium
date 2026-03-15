use std::cell::UnsafeCell;
use std::ffi::CString;
use std::mem::MaybeUninit;
use std::os::raw::{c_char, c_int};
use std::ptr;

use pyo3_ffi::*;

use crate::types::PyObjectPtr;

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

pub unsafe fn intern_str(s: *const c_char) -> *mut PyObject {
    unsafe { PyUnicode_InternFromString(s) }
}

/// Resolve a dotted path like "decimal.Decimal" or "xml.etree.ElementTree.Element".
///
/// First segment: try builtins, then import.
/// Subsequent segments: try getattr, then import accumulated dotted path.
pub unsafe fn resolve_path(path: &str) -> *mut PyObject {
    let segments: Vec<&str> = path.split('.').collect();
    if segments.is_empty() {
        unsafe {
            PyErr_SetString(
                PyExc_ValueError,
                b"cache::resolve_path: empty path\0".as_ptr().cast(),
            );
        }
        return ptr::null_mut();
    }

    let first = match CString::new(segments[0]) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };

    let mut cur: *mut PyObject;
    unsafe {
        let builtins = PyEval_GetBuiltins();
        let builtin_hit = if !builtins.is_null() {
            PyDict_GetItemString(builtins, first.as_ptr())
        } else {
            ptr::null_mut()
        };

        if !builtin_hit.is_null() {
            cur = builtin_hit.newref();
        } else {
            PyErr_Clear();
            cur = PyImport_ImportModule(first.as_ptr());
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
            let next = PyObject_GetAttrString(cur, seg.as_ptr());
            if !next.is_null() {
                cur.decref();
                cur = next;
                continue;
            }
            PyErr_Clear();

            let dotted: String = segments[..=i].join(".");
            if let Ok(module_path) = CString::new(dotted) {
                let module = PyImport_ImportModule(module_path.as_ptr());
                if !module.is_null() {
                    cur.decref();
                    cur = module;
                    continue;
                }
            }

            PyErr_Clear();
            let path_cstr = CString::new(path).unwrap_or_default();
            PyErr_Format(
                PyExc_AttributeError,
                b"cache: cannot resolve '%s' in '%s'\0".as_ptr().cast(),
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
        unsafe {
            PyErr_Clear();
        }
    }
    result
}

unsafe fn make_globals() -> *mut PyObject {
    unsafe {
        let globals = PyDict_New();
        if globals.is_null() {
            return ptr::null_mut();
        }
        let builtins = PyEval_GetBuiltins();
        if !builtins.is_null()
            && PyDict_SetItemString(globals, b"__builtins__\0".as_ptr().cast(), builtins) < 0
        {
            globals.decref();
            return ptr::null_mut();
        }
        globals
    }
}

pub unsafe fn eval_cstr(code: *const c_char) -> *mut PyObject {
    unsafe {
        let globals = make_globals();
        if globals.is_null() {
            return ptr::null_mut();
        }
        let result = PyRun_StringFlags(code, Py_eval_input, globals, globals, ptr::null_mut());
        globals.decref();
        result
    }
}

pub unsafe fn exec_cstr(code: *const c_char) -> *mut PyDictObject {
    unsafe {
        let globals = make_globals();
        if globals.is_null() {
            return ptr::null_mut();
        }
        let result = PyRun_StringFlags(code, Py_file_input, globals, globals, ptr::null_mut());
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
    unsafe { eval_cstr(code.as_ptr()) }
}

pub unsafe fn exec_str(s: &str) -> *mut PyDictObject {
    let code = dedent(s);
    unsafe { exec_cstr(code.as_ptr()) }
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
            let val = $crate::cache::intern_str(
                concat!($s, "\0").as_ptr() as *const ::std::os::raw::c_char
            );
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
        $crate::__py_obj_impl!(required, *mut ::pyo3_ffi::PyObject, $path)
    };
    (? $path:literal) => {
        $crate::__py_obj_impl!(optional, *mut ::pyo3_ffi::PyObject, $path)
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
                if ::pyo3_ffi::PyType_Check(val) == 0 {
                    ::pyo3_ffi::PyErr_SetString(
                        ::pyo3_ffi::PyExc_TypeError,
                        concat!("py_type!(\"", $path, "\"): resolved to non-type\0").as_ptr()
                            as *const ::std::os::raw::c_char,
                    );
                    $crate::types::PyObjectPtr::decref(val);
                    return -1;
                }
                SLOT.set(val);
                0
            }
        }

        ::inventory::submit! { $crate::cache::ObjEntry { init_fn: __init } }

        unsafe { SLOT.get() as *mut ::pyo3_ffi::PyTypeObject }
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
            let val: *mut ::pyo3_ffi::PyObject =
                (|| -> *mut ::pyo3_ffi::PyObject { unsafe { $($body)+ } })();
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
        SLOT.get()
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
