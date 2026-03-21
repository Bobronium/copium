use libc::c_ulong;
use pyo3_ffi::*;
use std::os::raw::c_int;

use super::ffi;

pub unsafe trait PyTypeObjectPtr {
    unsafe fn is_literal_immutable(self) -> bool;
    unsafe fn is_builtin_immutable(self) -> bool;
    unsafe fn is_stdlib_immutable(self) -> bool;
    unsafe fn is_type_subclass(self) -> bool;
    unsafe fn is_atomic_immutable(self) -> bool;
    unsafe fn is_immutable_collection(self) -> bool;
    unsafe fn ready(self) -> c_int;
}

unsafe impl PyTypeObjectPtr for *mut PyTypeObject {
    #[inline(always)]
    unsafe fn is_literal_immutable(self) -> bool {
        (self == std::ptr::addr_of_mut!(ffi::_PyNone_Type))
            | (self == std::ptr::addr_of_mut!(PyLong_Type))
            | (self == std::ptr::addr_of_mut!(PyUnicode_Type))
            | (self == std::ptr::addr_of_mut!(PyBool_Type))
            | (self == std::ptr::addr_of_mut!(PyFloat_Type))
            | (self == std::ptr::addr_of_mut!(PyBytes_Type))
    }

    #[inline(always)]
    unsafe fn is_builtin_immutable(self) -> bool {
        (self == std::ptr::addr_of_mut!(PyRange_Type))
            | (self == std::ptr::addr_of_mut!(PyFunction_Type))
            | (self == std::ptr::addr_of_mut!(PyCFunction_Type))
            | (self == std::ptr::addr_of_mut!(ffi::PyProperty_Type))
            | (self == std::ptr::addr_of_mut!(ffi::_PyWeakref_RefType))
            | (self == std::ptr::addr_of_mut!(PyCode_Type))
            | (self == std::ptr::addr_of_mut!(ffi::_PyNotImplemented_Type))
            | (self == std::ptr::addr_of_mut!(ffi::PyEllipsis_Type))
            | (self == std::ptr::addr_of_mut!(PyComplex_Type))
    }

    #[inline(always)]
    unsafe fn is_stdlib_immutable(self) -> bool {
        (self == crate::py_type!("re.Pattern"))
            || (self == crate::py_type!("decimal.Decimal"))
            || (self == crate::py_type!("fractions.Fraction"))
    }

    #[inline(always)]
    unsafe fn is_type_subclass(self) -> bool {
        (ffi::tp_flags_of(self) & (Py_TPFLAGS_TYPE_SUBCLASS as c_ulong)) != 0
    }

    #[inline(always)]
    unsafe fn is_atomic_immutable(self) -> bool {
        self.is_literal_immutable()
            || self.is_builtin_immutable()
            || self.is_type_subclass()
            || self.is_stdlib_immutable()
    }

    #[inline(always)]
    unsafe fn is_immutable_collection(self) -> bool {
        (self == std::ptr::addr_of_mut!(PyTuple_Type))
            | (self == std::ptr::addr_of_mut!(PyFrozenSet_Type))
            | (self == std::ptr::addr_of_mut!(PySlice_Type))
    }

    #[inline(always)]
    unsafe fn ready(self) -> c_int {
        pyo3_ffi::PyType_Ready(self)
    }
}
