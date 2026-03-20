use crate::deepcopy::PyResult;
use crate::py::{self, *};
use crate::reduce::{self, ReduceKind};
use crate::{py_str};
use std::os::raw::c_int;
use std::ptr;

macro_rules! check {
    ($expression:expr) => {{
        let pointer = $expression;
        if pointer.is_null() {
            return PyResult::error();
        }
        pointer
    }};
}

trait PyCopy {
    unsafe fn copy(self) -> PyResult;
}

pub unsafe fn copy(object: *mut PyObject) -> PyResult {
    unsafe {
        let cls = object.class();

        if cls.is_atomic_immutable() || cls.is_immutable_collection() || cls.is_type_subclass() {
            return PyResult::ok(object.newref());
        }

        if let Some(object) = PyListObject::cast_exact(object, cls) {
            return object.copy();
        }
        if let Some(object) = PyDictObject::cast_exact(object, cls) {
            return object.copy();
        }
        if let Some(object) = PySetObject::cast_exact(object, cls) {
            return object.copy();
        }
        if let Some(object) = PyByteArrayObject::cast_exact(object, cls) {
            return object.copy();
        }

        object.copy()
    }
}

impl PyCopy for *mut PyListObject {
    unsafe fn copy(self) -> PyResult {
        unsafe {
            let size = self.length();
            let copied = check!(py::list::new(size));

            for index in 0..size {
                let item = self.get_borrowed_unchecked(index);
                item.incref();
                copied.set_slot_steal_unchecked(index, item);
            }

            PyResult::ok(copied)
        }
    }
}

impl PyCopy for *mut PyDictObject {
    unsafe fn copy(self) -> PyResult {
        unsafe { PyResult::ok(check!(self.dict_copy())) }
    }
}

impl PyCopy for *mut PySetObject {
    unsafe fn copy(self) -> PyResult {
        unsafe { PyResult::ok(check!(py::set::from(self))) }
    }
}

impl PyCopy for *mut PyByteArrayObject {
    unsafe fn copy(self) -> PyResult {
        unsafe {
            let size = PyBufPtr::len(self);
            let copied = check!(py::bytearray::new(size));
            if size > 0 {
                ptr::copy_nonoverlapping(self.as_ptr(), copied.as_ptr(), size as usize);
            }
            PyResult::ok(copied)
        }
    }
}

impl PyCopy for *mut PyObject {
    unsafe fn copy(self) -> PyResult {
        unsafe {
            let mut custom_copy: *mut PyObject = ptr::null_mut();
            let has_custom_copy = self.get_optional_attr(py_str!("__copy__"), &mut custom_copy);
            if has_custom_copy < 0 {
                return PyResult::error();
            }
            if has_custom_copy > 0 {
                let copied = custom_copy.call();
                custom_copy.decref();
                if copied.is_null() {
                    return PyResult::error();
                }
                return PyResult::ok(copied);
            }

            copy_via_reduce(self)
        }
    }
}

unsafe fn reconstruct_shallow_instance(
    callable: *mut PyObject,
    arguments: *mut PyObject,
) -> *mut PyObject {
    unsafe {
        if (arguments as *mut PyTupleObject).length() == 0 {
            callable.call()
        } else {
            callable.call_with(arguments)
        }
    }
}

unsafe fn apply_setstate(instance: *mut PyObject, state: *mut PyObject) -> c_int {
    unsafe {
        let mut setstate: *mut PyObject = ptr::null_mut();
        if instance.get_optional_attr(py_str!("__setstate__"), &mut setstate) < 0 {
            return -1;
        }
        if setstate.is_null() {
            return 0;
        }

        let result = setstate.call_one(state);
        setstate.decref();
        if result.is_null() {
            return -1;
        }
        result.decref();
        1
    }
}

unsafe fn apply_dict_state(instance: *mut PyObject, dict_state: *mut PyObject) -> c_int {
    unsafe {
        if dict_state.is_null() || dict_state.is_none() {
            return 0;
        }

        if !dict_state.is_dict() {
            let instance_dict = instance.getattr(py_str!("__dict__"));
            if instance_dict.is_null() {
                return -1;
            }

            let result = (instance_dict as *mut PyDictObject).merge(dict_state, true);
            instance_dict.decref();
            if result < 0 {
                let message = py::unicode::from_format!(
                    crate::cstr!(
                        "dict state from %s.__reduce__ must be a dict or mapping, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*dict_state.class()).tp_name,
                );
                if !message.is_null() {
                    reduce::chain_type_error(message.as_object());
                }
            }
            return result;
        }

        let instance_dict = instance.getattr(py_str!("__dict__"));
        if instance_dict.is_null() {
            return -1;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut position: Py_ssize_t = 0;
        let mut result: c_int = 0;

        while (dict_state as *mut PyDictObject).dict_next(&mut position, &mut key, &mut value)
        {
            if instance_dict.setitem(key, value) < 0 {
                result = -1;
                break;
            }
        }

        instance_dict.decref();
        result
    }
}

unsafe fn apply_slot_state(instance: *mut PyObject, slot_state: *mut PyObject) -> c_int {
    unsafe {
        if slot_state.is_null() || slot_state.is_none() {
            return 0;
        }

        if !slot_state.is_dict() {
            let items_attribute = slot_state.getattr_cstr(crate::cstr!("items"));
            if items_attribute.is_null() {
                let message = py::unicode::from_format!(
                    crate::cstr!(
                        "slot state from %s.__reduce__ must be a dict or have an items() method, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*slot_state.class()).tp_name,
                );
                if !message.is_null() {
                    reduce::chain_type_error(message.as_object());
                }
                return -1;
            }

            let items = items_attribute.call();
            items_attribute.decref();
            if items.is_null() {
                return -1;
            }

            let iterator = items.get_iter();
            items.decref();
            if iterator.is_null() {
                return -1;
            }

            let mut result: c_int = 0;
            loop {
                let pair = iterator.iter_next();
                if pair.is_null() {
                    break;
                }

                let sequence = py::seq::fast(pair, crate::cstr!("items() must return pairs"));
                pair.decref();
                if sequence.is_null() {
                    result = -1;
                    break;
                }

                if py::seq::fast_length(sequence) != 2 {
                    sequence.decref();
                    if py::err::occurred().is_null() {
                        py::err::set_string(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack"),
                        );
                    }
                    result = -1;
                    break;
                }

                let key = py::seq::fast_borrow_item_unchecked(sequence, 0);
                let value = py::seq::fast_borrow_item_unchecked(sequence, 1);
                let set_result = instance.set_attr(key, value);
                sequence.decref();
                if set_result < 0 {
                    result = -1;
                    break;
                }
            }

            if result == 0 && !py::err::occurred().is_null() {
                result = -1;
            }
            iterator.decref();
            return result;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut position: Py_ssize_t = 0;
        let mut result: c_int = 0;

        while (slot_state as *mut PyDictObject).dict_next(&mut position, &mut key, &mut value)
        {
            if instance.set_attr(key, value) < 0 {
                result = -1;
                break;
            }
        }

        result
    }
}

unsafe fn apply_state_tuple(instance: *mut PyObject, state: *mut PyObject) -> c_int {
    unsafe {
        let mut dict_state = state;
        let mut slot_state: *mut PyObject = ptr::null_mut();

        if state.is_tuple() && (state as *mut PyTupleObject).length() == 2 {
            let tuple = state as *mut PyTupleObject;
            dict_state = tuple.get_borrowed_unchecked(0);
            slot_state = tuple.get_borrowed_unchecked(1);
        }

        if apply_dict_state(instance, dict_state) < 0 {
            return -1;
        }
        apply_slot_state(instance, slot_state)
    }
}

unsafe fn apply_listitems(instance: *mut PyObject, listitems: *mut PyObject) -> c_int {
    unsafe {
        if listitems.is_null() {
            return 0;
        }

        let append = instance.getattr(py_str!("append"));
        if append.is_null() {
            return -1;
        }

        let iterator = listitems.get_iter();
        if iterator.is_null() {
            append.decref();
            return -1;
        }

        let mut result: c_int = 0;
        loop {
            let item = iterator.iter_next();
            if item.is_null() {
                break;
            }

            let append_result = append.call_one(item);
            item.decref();
            if append_result.is_null() {
                result = -1;
                break;
            }
            append_result.decref();
        }

        if result == 0 && !py::err::occurred().is_null() {
            result = -1;
        }
        iterator.decref();
        append.decref();
        result
    }
}

unsafe fn apply_dictitems(instance: *mut PyObject, dictitems: *mut PyObject) -> c_int {
    unsafe {
        if dictitems.is_null() {
            return 0;
        }

        let iterator = dictitems.get_iter();
        if iterator.is_null() {
            return -1;
        }

        let mut result: c_int = 0;
        loop {
            let pair = iterator.iter_next();
            if pair.is_null() {
                break;
            }

            let (key, value) = if pair.is_tuple() && (pair as *mut PyTupleObject).length() == 2 {
                let pair_tuple = pair as *mut PyTupleObject;
                (
                    pair_tuple.get_borrowed_unchecked(0).newref(),
                    pair_tuple.get_borrowed_unchecked(1).newref(),
                )
            } else {
                let sequence = py::seq::fast(pair, crate::cstr!("cannot unpack non-sequence"));
                if sequence.is_null() {
                    pair.decref();
                    result = -1;
                    break;
                }

                let item_count = py::seq::fast_length(sequence);
                if item_count != 2 {
                    sequence.decref();
                    pair.decref();
                    if item_count < 2 {
                        py::err::format!(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack (expected 2, got %zd)"),
                            item_count,
                        );
                    } else {
                        py::err::format!(
                            PyExc_ValueError,
                            crate::cstr!("too many values to unpack (expected 2, got %zd)"),
                            item_count,
                        );
                    }
                    result = -1;
                    break;
                }

                let key = py::seq::fast_borrow_item_unchecked(sequence, 0).newref();
                let value = py::seq::fast_borrow_item_unchecked(sequence, 1).newref();
                sequence.decref();
                (key, value)
            };
            pair.decref();

            let set_result = instance.setitem(key, value);
            key.decref();
            value.decref();
            if set_result < 0 {
                result = -1;
                break;
            }
        }

        if result == 0 && !py::err::occurred().is_null() {
            result = -1;
        }
        iterator.decref();
        result
    }
}

unsafe fn reconstruct_state(
    instance: *mut PyObject,
    state: *mut PyObject,
    listitems: *mut PyObject,
    dictitems: *mut PyObject,
) -> c_int {
    unsafe {
        if !state.is_null() && !state.is_none() {
            let applied = apply_setstate(instance, state);
            if applied < 0 {
                return -1;
            }
            if applied == 0 && apply_state_tuple(instance, state) < 0 {
                return -1;
            }
        }

        if apply_listitems(instance, listitems) < 0 {
            return -1;
        }

        apply_dictitems(instance, dictitems)
    }
}

unsafe fn copy_via_reduce(object: *mut PyObject) -> PyResult {
    unsafe {
        let class = object.class();

        let mut reduce_result = reduce::try_reduce_via_registry(object, class);
        if reduce_result.is_null() {
            if !py::err::occurred().is_null() {
                return PyResult::error();
            }

            reduce_result = reduce::call_reduce_method_preferring_ex(object);
            if reduce_result.is_null() {
                return PyResult::error();
            }
        }

        let (kind, parts) = reduce::validate_reduce_tuple(reduce_result, class);

        match kind {
            ReduceKind::Error => {
                reduce_result.decref();
                PyResult::error()
            }
            ReduceKind::String => {
                reduce_result.decref();
                PyResult::ok(object.newref())
            }
            ReduceKind::Tuple => {
                let copied = reconstruct_shallow_instance(parts.callable, parts.argtup);
                if copied.is_null() {
                    reduce_result.decref();
                    return PyResult::error();
                }

                if reconstruct_state(copied, parts.state, parts.listitems, parts.dictitems) < 0 {
                    copied.decref();
                    reduce_result.decref();
                    return PyResult::error();
                }

                reduce_result.decref();
                PyResult::ok(copied)
            }
        }
    }
}
