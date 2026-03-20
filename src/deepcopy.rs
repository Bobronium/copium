use std::hint::{likely, unlikely};
use std::ptr;

use crate::dict_iter::DictIterGuard;
use crate::memo::Memo;
use crate::py::{self, *};
use crate::py_str;

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct PyResult(pub *mut PyObject);

impl PyResult {
    #[inline(always)]
    pub fn ok<T: PyTypeInfo>(p: *mut T) -> Self {
        Self(p.cast())
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

macro_rules! protect_stack {
    ($expr:expr) => {{
        if unlikely(crate::recursion::enter() < 0) {
            return PyResult::error();
        }
        let result = $expr;
        crate::recursion::leave();
        result
    }};
}

#[inline(always)]
unsafe fn is_prememo_atomic<M: Memo>(cls: *mut PyTypeObject) -> bool {
    // NativeMemo
    if !M::RECALL_CAN_ERROR {
        return cls.is_literal_immutable();
    }

    // AnyMemo or DictMemo

    // Python 3.14 checks atomics before checking memo, so we can do it here
    #[cfg(Py_3_14)]
    {
        cls.is_literal_immutable() || cls.is_builtin_immutable() || cls.is_type_subclass()
    }
    // Python < 3.14 doesn't check for atomic until it first checks memo
    // since AnyMemo/DiceMemo can error, in order to preserve the semantics
    // don't short circuit if it's atomic and let memo lookup potentially fail
    #[cfg(not(Py_3_14))]
    {
        let _ = cls;
        false
    }
}

#[inline(always)]
unsafe fn is_postmemo_atomic<M: Memo>(cls: *mut PyTypeObject) -> bool {
    if !M::RECALL_CAN_ERROR {
        return cls.is_builtin_immutable() || cls.is_type_subclass() || cls.is_stdlib_immutable();
    }

    #[cfg(Py_3_14)]
    {
        cls.is_stdlib_immutable()
    }

    #[cfg(not(Py_3_14))]
    {
        cls.is_atomic_immutable()
    }
}

#[inline(always)]
pub unsafe fn deepcopy<M: Memo>(object: *mut PyObject, memo: &mut M) -> PyResult {
    unsafe {
        let cls = object.class();

        if likely(is_prememo_atomic::<M>(cls)) {
            return PyResult::ok(object.newref());
        }

        let (probe, found) = memo.recall(object);
        if !found.is_null() {
            return PyResult::ok(found);
        }
        if M::RECALL_CAN_ERROR && unlikely(!py::err::occurred().is_null()) {
            return PyResult::error();
        }

        if let Some(object) = PyTupleObject::cast_exact(object, cls) {
            return protect_stack!(object.deepcopy(memo, probe));
        }
        if let Some(object) = PyDictObject::cast_exact(object, cls) {
            return protect_stack!(object.deepcopy(memo, probe));
        }
        if let Some(object) = PyListObject::cast_exact(object, cls) {
            return protect_stack!(object.deepcopy(memo, probe));
        }
        if let Some(object) = PySetObject::cast_exact(object, cls) {
            return protect_stack!(object.deepcopy(memo, probe));
        }

        if unlikely(is_postmemo_atomic::<M>(cls)) {
            return PyResult::ok(object.newref());
        }

        if let Some(object) = PyFrozensetObject::cast_exact(object, cls) {
            return protect_stack!(object.deepcopy(memo, probe));
        }
        if let Some(object) = PyByteArrayObject::cast_exact(object, cls) {
            return object.deepcopy(memo, probe);
        }
        if let Some(object) = PyMethodObject::cast_exact(object, cls) {
            return object.deepcopy(memo, probe);
        }

        object.deepcopy(memo, probe)
    }
}

impl PyDeepCopy for *mut PyListObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.length();
            let copied = check!(py::list::new(sz));

            let ellipsis = py::EllipsisObject;
            for i in 0..sz {
                #[cfg(not(Py_3_12))]
                ellipsis.incref();
                copied.set_slot_steal_unchecked(i, ellipsis);
            }

            if memo.memoize(self, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            for i in 0..sz {
                let item = self.get_owned_check_bounds(i);
                if unlikely(item.is_null()) {
                    py::err::set_string(
                        PyExc_RuntimeError,
                        crate::cstr!("list changed size during iteration"),
                    );
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let item_copy = deepcopy(item, memo);
                item.decref();

                if unlikely(item_copy.is_error()) {
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let raw = item_copy.into_raw();
                let mut size_changed = false;
                py::critical_section::enter(copied, || {
                    if unlikely(copied.length() != sz) {
                        size_changed = true;
                    } else {
                        #[cfg(not(Py_3_12))]
                        let old_item = copied.get_borrowed_unchecked(i);
                        copied.set_slot_steal_unchecked(i, raw);
                        #[cfg(not(Py_3_12))]
                        old_item.decref();
                    }
                });
                if unlikely(size_changed) {
                    raw.decref();
                    py::err::set_string(
                        PyExc_RuntimeError,
                        crate::cstr!("list changed size during iteration"),
                    );
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }
            }

            PyResult::ok(copied)
        }
    }
}

impl PyDeepCopy for *mut PyTupleObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let sz = self.length();
            let copied = check!(py::tuple::new(sz));

            let mut all_same = true;
            for i in 0..sz {
                let item = self.get_borrowed_unchecked(i);
                let item_copy = deepcopy(item, memo);
                if unlikely(item_copy.is_error()) {
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

            let existing = memo.recall_probed(self, &probe);
            if unlikely(!existing.is_null()) {
                copied.decref();
                return PyResult::ok(existing);
            }

            if memo.memoize(self, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied)
        }
    }
}

impl PyDeepCopy for *mut PyDictObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let copied = check!(py::dict::new_presized(self.len()));

            if memo.memoize(self, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            let mut guard = DictIterGuard::new(self);
            guard.activate();
            let mut key: *mut PyObject = ptr::null_mut();
            let mut value: *mut PyObject = ptr::null_mut();

            loop {
                let flag = guard.next(&mut key, &mut value);
                if flag == 0 {
                    break;
                }
                if flag < 0 {
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let key_copy = deepcopy(key, memo);
                key.decref();
                if unlikely(key_copy.is_error()) {
                    value.decref();
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let val_copy = deepcopy(value, memo);
                value.decref();
                if unlikely(val_copy.is_error()) {
                    key_copy.into_raw().decref();
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }

                let rc = copied.set_item_steal_two(key_copy.into_raw(), val_copy.into_raw());

                if unlikely(rc < 0) {
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }
            }

            PyResult::ok(copied)
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
            let snapshot = check!(py::tuple::new(sz));

            let mut i: Py_ssize_t = 0;
            py::critical_section::enter(self, || {
                let mut pos: Py_ssize_t = 0;
                let mut item: *mut PyObject = ptr::null_mut();
                let mut hash: Py_hash_t = 0;
                while self.next_entry(&mut pos, &mut item, &mut hash) {
                    item.incref();
                    snapshot.set_slot_steal_unchecked(i, item);
                    i += 1;
                }
            });

            let copied = py::set::new();
            if copied.is_null() {
                snapshot.decref();
                return PyResult::error();
            }

            if memo.memoize(self, copied, &probe) < 0 {
                snapshot.decref();
                copied.decref();
                return PyResult::error();
            }

            for j in 0..i {
                let item = snapshot.get_borrowed_unchecked(j);
                let item_copy = deepcopy(item, memo);
                if item_copy.is_error() {
                    snapshot.decref();
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }
                let raw = item_copy.into_raw();
                let rc = copied.add_item(raw);
                raw.decref();
                if rc < 0 {
                    snapshot.decref();
                    memo.forget(self, &probe);
                    copied.decref();
                    return PyResult::error();
                }
            }

            snapshot.decref();
            PyResult::ok(copied)
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

            let snapshot = check!(py::tuple::new(sz));

            let mut pos: Py_ssize_t = 0;
            let mut item: *mut PyObject = ptr::null_mut();
            let mut hash: Py_hash_t = 0;
            let mut i: Py_ssize_t = 0;
            while self.next_entry(&mut pos, &mut item, &mut hash) {
                item.incref();
                snapshot.set_slot_steal_unchecked(i, item);
                i += 1;
            }

            let items = py::tuple::new(i);
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

            let copied = py::set::frozen_from(items);
            items.decref();
            if copied.is_null() {
                return PyResult::error();
            }

            if memo.memoize(self, copied, &probe) < 0 {
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
            let copied = check!(py::bytearray::new(sz));

            if sz > 0 {
                ptr::copy_nonoverlapping(self.as_ptr(), copied.as_ptr(), sz as usize);
            }

            if memo.memoize(self, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied)
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

            let copied = py::method::new(func, copied_self_raw);
            copied_self_raw.decref();

            if copied.is_null() {
                return PyResult::error();
            }

            if memo.memoize(self, copied, &probe) < 0 {
                copied.decref();
                return PyResult::error();
            }

            PyResult::ok(copied)
        }
    }
}

impl PyDeepCopy for *mut PyObject {
    unsafe fn deepcopy<M: Memo>(self, memo: &mut M, probe: M::Probe) -> PyResult {
        unsafe {
            let mut custom_deepcopy_method: *mut PyObject = ptr::null_mut();
            let has = self.get_optional_attr(py_str!("__deepcopy__"), &mut custom_deepcopy_method);
            if has < 0 {
                return PyResult::error();
            }
            if has > 0 {
                return deepcopy_custom(self, custom_deepcopy_method, memo, probe);
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

unsafe fn deepcopy_custom<M: Memo>(
    object: *mut PyObject,
    custom_deepcopy_method: *mut PyObject,
    memo: &mut M,
    probe: M::Probe,
) -> PyResult {
    unsafe {
        let checkpoint = memo.checkpoint();
        let mut copied = custom_deepcopy_method.call_one(memo.as_call_arg());

        if copied.is_null() {
            if let Some(saved_checkpoint) = checkpoint {
                let native_memo = memo.as_native_memo();
                if !native_memo.is_null() {
                    copied = crate::fallback::maybe_retry_with_dict_memo(
                        object,
                        custom_deepcopy_method,
                        &mut *native_memo,
                        saved_checkpoint,
                    );
                }
            }
        }

        custom_deepcopy_method.decref();

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
