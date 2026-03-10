use pyo3_ffi::*;
use std::ptr;

use crate::compat;
use crate::critical_section::with_critical_section_raw;
use crate::dict_iter::DictIterGuard;
use crate::ffi_ext::*;
use crate::memo::{Memo, MemoCheckpoint};
use crate::recursion;
use crate::state::STATE;
use crate::types::*;

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct PyResult(pub *mut PyObject);

impl PyResult {
    #[inline(always)]
    pub fn ok(p: *mut PyObject) -> Self {
        Self(p)
    }
    #[inline(always)]
    pub fn error() -> Self {
        Self(ptr::null_mut())
    }
    #[inline(always)]
    pub fn is_error(self) -> bool {
        self.0.is_null()
    }
    #[inline(always)]
    pub fn into_raw(self) -> *mut PyObject {
        self.0
    }
}

macro_rules! check {
    ($e:expr) => {{
        let p = $e;
        if p.is_null() {
            return PyResult::error();
        }
        p
    }};
}

trait PyDeepCopy {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult;
}

#[inline(always)]
unsafe fn dispatch<T, M: Memo>(object: *mut T, memo: &mut M, probe: M::Probe) -> PyResult
where
    *mut T: PyDeepCopy,
{
    if unsafe { recursion::enter() } < 0 {
        return PyResult::error();
    }
    let result = unsafe { object.deepcopy(memo, probe) };
    recursion::leave();
    result
}

#[inline(always)]
pub unsafe fn deepcopy<M: Memo>(object: *mut PyObject, memo: &mut M) -> PyResult {
    unsafe {
        let cls = object.class();

        if cls.is_literal_immutable() || cls.is_builtin_immutable() || cls.is_type_subclass() {
            return PyResult::ok(object.newref());
        }

        deepcopy_cold(object, cls, memo)
    }
}

#[inline(never)]
unsafe fn deepcopy_cold<M: Memo>(
    object: *mut PyObject,
    cls: *mut PyTypeObject,
    memo: &mut M,
) -> PyResult {
    unsafe {
        let (probe, found) = memo.recall(object);
        if !found.is_null() {
            return PyResult::ok(found);
        }
        if M::RECALL_CAN_ERROR && !PyErr_Occurred().is_null() {
            return PyResult::error();
        }

        if cls == PyTupleObject::type_ptr() {
            return dispatch(object as *mut PyTupleObject, memo, probe);
        }
        if cls == PyDictObject::type_ptr() {
            return dispatch(object as *mut PyDictObject, memo, probe);
        }
        if cls == PyListObject::type_ptr() {
            return dispatch(object as *mut PyListObject, memo, probe);
        }
        if cls == PySetObject::type_ptr() {
            return dispatch(object as *mut PySetObject, memo, probe);
        }
        if cls == PyFrozensetObject::type_ptr() {
            return dispatch(object as *mut PyFrozensetObject, memo, probe);
        }
        if cls == PyByteArrayObject::type_ptr() {
            return (object as *mut PyByteArrayObject).deepcopy(memo, probe);
        }
        if cls == PyMethodObject::type_ptr() {
            return (object as *mut PyMethodObject).deepcopy(memo, probe);
        }

        if cls.is_stdlib_immutable() {
            return PyResult::ok(object.newref());
        }

        object.deepcopy(memo, probe)
    }
}

impl PyDeepCopy for *mut PyListObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.len();
            let copied = check!(py_list_new(sz));

            for i in 0..sz {
                let ellipsis = Py_Ellipsis();
                #[cfg(not(any(Py_3_12, Py_3_12, Py_3_13, Py_3_14)))]
                ellipsis.incref();
                copied.set_slot_steal_unchecked(i, ellipsis);
            }

            if memo.memoize(self as _, copied as _, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            for i in 0..sz {
                let item = self.get_owned_check_bounds(i);
                if item.is_null() {
                    PyErr_SetString(
                        PyExc_RuntimeError,
                        crate::cstr!("list changed size during iteration"),
                    );
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let item_copy = deepcopy(item, memo);
                item.decref();

                if item_copy.is_error() {
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let raw = item_copy.into_raw();
                let mut size_changed = false;
                with_critical_section_raw(copied as _, || {
                    if copied.len() != sz {
                        size_changed = true;
                    } else {
                        #[cfg(not(any(Py_3_12, Py_3_13, Py_3_14)))]
                        let old_item = copied.get_borrowed(i);
                        copied.set_slot_steal_unchecked(i, raw);
                        #[cfg(not(any(Py_3_12, Py_3_13, Py_3_14)))]
                        old_item.decref();
                    }
                });
                if size_changed {
                    raw.decref();
                    PyErr_SetString(
                        PyExc_RuntimeError,
                        crate::cstr!("list changed size during iteration"),
                    );
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }
            }

            PyResult::ok(copied as _)
        }
    }
}

impl PyDeepCopy for *mut PyTupleObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.len();
            let copied = check!(py_tuple_new(sz));

            let mut all_same = true;
            for i in 0..sz {
                let item = self.get_borrowed_unchecked(i);
                let item_copy = deepcopy(item, memo);
                if item_copy.is_error() {
                    copied.decref();
                    return PyResult::error();
                }
                let raw = item_copy.into_raw();
                if raw != item {
                    all_same = false;
                }
                copied.set_slot_steal_unchecked(i, raw);
            }

            if all_same {
                copied.decref();
                return PyResult::ok(self.newref());
            }

            let (_, existing) = memo.recall(self as _);
            if !existing.is_null() {
                copied.decref();
                return PyResult::ok(existing);
            }

            if memo.memoize(self as _, copied as _, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied as _)
        }
    }
}

impl PyDeepCopy for *mut PyDictObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let copied = check!(py_dict_new(self.len()));

            if memo.memoize(self as _, copied as _, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            let mut guard = DictIterGuard::new(self as _);
            guard.activate();
            let mut key: *mut PyObject = ptr::null_mut();
            let mut value: *mut PyObject = ptr::null_mut();

            loop {
                let flag = guard.next(&mut key, &mut value);
                if flag == 0 {
                    break;
                }
                if flag < 0 {
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let key_copy = deepcopy(key, memo);
                key.decref();
                if key_copy.is_error() {
                    value.decref();
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let val_copy = deepcopy(value, memo);
                value.decref();
                if val_copy.is_error() {
                    key_copy.into_raw().decref();
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let kraw = key_copy.into_raw();
                let vraw = val_copy.into_raw();
                let rc = copied.set_item(kraw, vraw);
                kraw.decref();
                vraw.decref();

                if rc < 0 {
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }
            }

            PyResult::ok(copied as _)
        }
    }
}

impl PyDeepCopy for *mut PySetObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.len();
            if sz < 0 {
                return PyResult::error();
            }
            let snapshot = check!(py_tuple_new(sz));

            let mut i: Py_ssize_t = 0;
            with_critical_section_raw(self as _, || {
                let mut pos: Py_ssize_t = 0;
                let mut item: *mut PyObject = ptr::null_mut();
                let mut hash: Py_hash_t = 0;
                while self.next_entry(&mut pos, &mut item, &mut hash) != 0 {
                    item.incref();
                    snapshot.set_slot_steal_unchecked(i, item);
                    i += 1;
                }
            });

            let copied = py_set_new();
            if copied.is_null() {
                snapshot.decref();
                return PyResult::error();
            }

            if memo.memoize(self as _, copied as _, &probe) < 0 {
                snapshot.decref();
                copied.decref();
                return PyResult::error();
            }

            for j in 0..i {
                let item = snapshot.get_borrowed_unchecked(j);
                let item_copy = deepcopy(item, memo);
                if item_copy.is_error() {
                    snapshot.decref();
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }
                let raw = item_copy.into_raw();
                let rc = copied.add_item(raw);
                raw.decref();
                if rc < 0 {
                    snapshot.decref();
                    memo.forget(self as _, &probe);
                    copied.decref();
                    return PyResult::error();
                }
            }

            snapshot.decref();
            PyResult::ok(copied as _)
        }
    }
}

impl PyDeepCopy for *mut PyFrozensetObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.len();
            if sz < 0 {
                return PyResult::error();
            }

            // stdlib exercises memo usability before reconstructing frozenset
            // members via the reduce-style path, so malformed mappings must
            // fail here rather than later in nested copies.
            if memo.ensure_memo_is_valid() < 0 {
                return PyResult::error();
            }

            let snapshot = check!(py_tuple_new(sz));

            let mut pos: Py_ssize_t = 0;
            let mut item: *mut PyObject = ptr::null_mut();
            let mut hash: Py_hash_t = 0;
            let mut i: Py_ssize_t = 0;
            while self.next_entry(&mut pos, &mut item, &mut hash) != 0 {
                item.incref();
                snapshot.set_slot_steal_unchecked(i, item);
                i += 1;
            }

            let items = py_tuple_new(i);
            if items.is_null() {
                snapshot.decref();
                return PyResult::error();
            }

            for j in 0..i {
                let orig = snapshot.get_borrowed_unchecked(j);
                let item_copy = deepcopy(orig, memo);
                if item_copy.is_error() {
                    snapshot.decref();
                    items.decref();
                    return PyResult::error();
                }
                items.set_slot_steal_unchecked(j, item_copy.into_raw());
            }
            snapshot.decref();

            let copied = frozenset_from(items as _);
            items.decref();
            if copied.is_null() {
                return PyResult::error();
            }

            if memo.memoize(self as _, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied)
        }
    }
}

impl PyDeepCopy for *mut PyByteArrayObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.len();
            let copied = check!(py_bytearray_new(sz));

            if sz > 0 {
                ptr::copy_nonoverlapping(self.as_ptr(), copied.as_ptr(), sz as usize);
            }

            if memo.memoize(self as _, copied as _, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied as _)
        }
    }
}

impl PyDeepCopy for *mut PyMethodObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let func = self.function();
            let instance = self.self_obj();

            let copied_self = deepcopy(instance, memo);

            if copied_self.is_error() {
                return PyResult::error();
            }
            let copied_self_raw = copied_self.into_raw();

            let copied = py_method_new(func, copied_self_raw);
            copied_self_raw.decref();

            if copied.is_null() {
                return PyResult::error();
            }

            if memo.memoize(self as _, copied as _, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied as _)
        }
    }
}

impl PyDeepCopy for *mut PyObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let s = &STATE;
            let mut dunder: *mut PyObject = ptr::null_mut();
            let has = self.get_optional_attr(s.s_deepcopy, &mut dunder);
            if has < 0 {
                return PyResult::error();
            }
            if has > 0 {
                return deepcopy_via_dunder(self, dunder, memo, probe);
            }

            let result = crate::reduce::reconstruct(self, self.class(), memo, probe);
            if result.is_null() {
                PyResult::error()
            } else {
                PyResult::ok(result)
            }
        }
    }
}

unsafe fn deepcopy_via_dunder<M: Memo>(
    object: *mut PyObject,
    dunder: *mut PyObject,
    memo: &mut M,
    probe: M::Probe,
) -> PyResult {
    unsafe {
        let checkpoint = memo.checkpoint();
        let mut copied = dunder.call_one(memo.as_call_arg());

        if copied.is_null() {
            if let Some(saved_checkpoint) = checkpoint {
                let native_memo = memo.as_native_memo();
                if !native_memo.is_null() {
                    copied = crate::fallback::maybe_retry_with_dict_memo(
                        object,
                        dunder,
                        &mut *native_memo,
                        saved_checkpoint,
                    );
                }
            }
        }

        dunder.decref();

        if copied.is_null() {
            return PyResult::error();
        }

        if copied != object {
            if memo.memoize(object, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }
        }

        PyResult::ok(copied)
    }
}
