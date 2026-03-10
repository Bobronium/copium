use core::ffi::c_char;
use pyo3_ffi::*;

#[cfg(Py_GIL_DISABLED)]
use core::sync::atomic::Ordering;

// ── Symbols not (reliably) in pyo3-ffi ──────────────────────

extern "C" {
    pub static mut _Py_NoneStruct: PyObject;
    pub static mut _Py_NotImplementedStruct: PyObject;
    pub static mut _Py_EllipsisObject: PyObject;
    pub static mut PyMethod_Type: PyTypeObject;
    pub static mut _PyNone_Type: PyTypeObject;
    pub static mut _PyNotImplemented_Type: PyTypeObject;
    pub static mut PyEllipsis_Type: PyTypeObject;
    pub static mut PyProperty_Type: PyTypeObject;
    pub static mut _PyWeakref_RefType: PyTypeObject;
}

/// PyMethod_Function is a macro in CPython; access via struct layout.
#[repr(C)]
pub struct PyMethodObject {
    pub ob_refcnt: pyo3_ffi::Py_ssize_t,
    pub ob_type: *mut PyTypeObject,
    pub im_func: *mut PyObject,
    pub im_self: *mut PyObject,
}

#[inline(always)]
pub unsafe fn PyMethod_GET_FUNCTION(m: *mut PyObject) -> *mut PyObject {
    unsafe { (*(m as *mut PyMethodObject)).im_func }
}

#[inline(always)]
pub unsafe fn PyMethod_GET_SELF(m: *mut PyObject) -> *mut PyObject {
    unsafe { (*(m as *mut PyMethodObject)).im_self }
}

/// PyMethod_New IS an exported function.
extern "C" {
    pub fn PyMethod_New(func: *mut PyObject, self_: *mut PyObject) -> *mut PyObject;
}

#[inline(always)]
pub unsafe fn Py_NotImplemented() -> *mut PyObject {
    unsafe { std::ptr::addr_of_mut!(_Py_NotImplementedStruct) }
}

#[inline(always)]
pub unsafe fn Py_None() -> *mut PyObject {
    unsafe { std::ptr::addr_of_mut!(_Py_NoneStruct) }
}

// ── Variadic FFI not reliably in pyo3-ffi ───────────────────

extern "C" {
    pub fn PyErr_Format(exception: *mut PyObject, format: *const c_char, ...) -> *mut PyObject;
    pub fn PyUnicode_FromFormat(format: *const c_char, ...) -> *mut PyObject;
    pub fn PySequence_Fast(o: *mut PyObject, m: *const c_char) -> *mut PyObject;
}

#[inline(always)]
pub unsafe fn PySequence_Fast_GET_SIZE(o: *mut PyObject) -> Py_ssize_t {
    unsafe { Py_SIZE(o) }
}

#[cfg(Py_GIL_DISABLED)]
#[inline(always)]
pub unsafe fn tp_flags_of(tp: *mut PyTypeObject) -> u64 {
    unsafe { (*tp).tp_flags.load(Ordering::Relaxed) }
}

#[cfg(not(Py_GIL_DISABLED))]
#[inline(always)]
pub unsafe fn tp_flags_of(tp: *mut PyTypeObject) -> u64 {
    unsafe { (*tp).tp_flags }
}

#[inline(always)]
pub unsafe fn PySequence_Fast_GET_ITEM(o: *mut PyObject, i: Py_ssize_t) -> *mut PyObject {
    unsafe {
        if (tp_flags_of(Py_TYPE(o)) & Py_TPFLAGS_LIST_SUBCLASS) != 0 {
            *(*(o as *mut PyListObject)).ob_item.add(i as usize)
        } else {
            *(*(o as *mut PyTupleObject)).ob_item.as_ptr().add(i as usize)
        }
    }
}

// ── Vectorcall helpers (static inline in CPython — must be reimplemented) ──

/// Equivalent to CPython's static inline PyVectorcall_Function.
/// Reads the vectorcall slot via tp_vectorcall_offset.
#[inline(always)]
pub unsafe fn PyVectorcall_Function(callable: *mut PyObject) -> Option<vectorcallfunc> {
    unsafe {
        let tp = Py_TYPE(callable);
        if tp_flags_of(tp) & Py_TPFLAGS_HAVE_VECTORCALL == 0 {
            return None;
        }
        let offset = (*tp).tp_vectorcall_offset;
        debug_assert!(offset > 0);
        let slot = (callable as *const u8).add(offset as usize) as *const *const ();
        let ptr = *slot;
        if ptr.is_null() {
            return None;
        }
        Some(std::mem::transmute::<*const (), vectorcallfunc>(ptr))
    }
}

/// Equivalent to CPython's static inline PyFunction_SetVectorcall (3.12+).
/// Writes the vectorcall slot via tp_vectorcall_offset.
#[cfg(Py_3_12)]
#[inline(always)]
pub unsafe fn set_fn_vectorcall(fn_obj: *mut PyObject, vc: vectorcallfunc) {
    unsafe {
        let tp = Py_TYPE(fn_obj);
        debug_assert!(tp_flags_of(tp) & Py_TPFLAGS_HAVE_VECTORCALL != 0);
        let offset = (*tp).tp_vectorcall_offset;
        debug_assert!(offset > 0);
        let slot = (fn_obj as *mut u8).add(offset as usize) as *mut vectorcallfunc;
        *slot = vc;
    }
}

/// Strips PY_VECTORCALL_ARGUMENTS_OFFSET from nargsf.
#[inline(always)]
pub fn PyVectorcall_NARGS(nargsf: usize) -> Py_ssize_t {
    const OFFSET_BIT: usize = 1usize << (usize::BITS - 1);
    (nargsf & !OFFSET_BIT) as Py_ssize_t
}

// ── C string literal helper ─────────────────────────────────

#[macro_export]
macro_rules! cstr {
    ($s:literal) => {
        concat!($s, "\0").as_ptr() as *const core::ffi::c_char
    };
}
