use pyo3_ffi::*;
use std::os::raw::c_int;
use std::ptr;

use crate::deepcopy::PyResult;
use crate::ffi_ext;
use crate::reduce::{self, ReduceKind};
use crate::state::STATE;
use crate::types::*;

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
        let class = object.class();

        if class.is_atomic_immutable() || class.is_stdlib_immutable() {
            return PyResult::ok(object.newref());
        }

        if class == PyTupleObject::type_ptr()
            || class == PyFrozensetObject::type_ptr()
            || PySlice_Check(object) != 0
            || PyType_IsSubtype(class, std::ptr::addr_of_mut!(PyType_Type)) != 0
        {
            return PyResult::ok(object.newref());
        }

        if class == PyListObject::type_ptr() {
            return (object as *mut PyListObject).copy();
        }
        if class == PyDictObject::type_ptr() {
            return (object as *mut PyDictObject).copy();
        }
        if class == PySetObject::type_ptr() {
            return (object as *mut PySetObject).copy();
        }
        if class == PyByteArrayObject::type_ptr() {
            return (object as *mut PyByteArrayObject).copy();
        }

        object.copy()
    }
}

impl PyCopy for *mut PyListObject {
    unsafe fn copy(self) -> PyResult {
        unsafe {
            let size = self.length();
            let copied = check!(py_list_new(size));

            for index in 0..size {
                let item = self.get_borrowed_unchecked(index);
                item.incref();
                copied.set_slot_steal_unchecked(index, item);
            }

            PyResult::ok(copied as _)
        }
    }
}

impl PyCopy for *mut PyDictObject {
    unsafe fn copy(self) -> PyResult {
        unsafe { PyResult::ok(check!(PyDict_Copy(self as *mut PyObject))) }
    }
}

impl PyCopy for *mut PySetObject {
    unsafe fn copy(self) -> PyResult {
        unsafe { PyResult::ok(check!(PySet_New(self as *mut PyObject))) }
    }
}

impl PyCopy for *mut PyByteArrayObject {
    unsafe fn copy(self) -> PyResult {
        unsafe {
            let size = crate::types::PyBufPtr::len(self);
            let copied = check!(py_bytearray_new(size));
            if size > 0 {
                ptr::copy_nonoverlapping(self.as_ptr(), copied.as_ptr(), size as usize);
            }
            PyResult::ok(copied as _)
        }
    }
}

impl PyCopy for *mut PyObject {
    unsafe fn copy(self) -> PyResult {
        unsafe {
            let mut dunder_copy: *mut PyObject = ptr::null_mut();
            let has_dunder_copy = self.get_optional_attr(STATE.s_copy, &mut dunder_copy);
            if has_dunder_copy < 0 {
                return PyResult::error();
            }
            if has_dunder_copy > 0 {
                let copied = dunder_copy.call();
                dunder_copy.decref();
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
        if instance.get_optional_attr(STATE.s_setstate, &mut setstate) < 0 {
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
            let instance_dict = instance.getattr(STATE.s_dict);
            if instance_dict.is_null() {
                return -1;
            }

            let result = PyDict_Merge(instance_dict, dict_state, 1);
            instance_dict.decref();
            if result < 0 {
                let message = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "dict state from %s.__reduce__ must be a dict or mapping, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*dict_state.class()).tp_name,
                );
                if !message.is_null() {
                    reduce::chain_type_error(message);
                }
            }
            return result;
        }

        let instance_dict = instance.getattr(STATE.s_dict);
        if instance_dict.is_null() {
            return -1;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut position: Py_ssize_t = 0;
        let mut result: c_int = 0;

        while PyDict_Next(dict_state, &mut position, &mut key, &mut value) != 0 {
            if PyObject_SetItem(instance_dict, key, value) < 0 {
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
            let items_attribute = PyObject_GetAttrString(slot_state, crate::cstr!("items"));
            if items_attribute.is_null() {
                let message = ffi_ext::PyUnicode_FromFormat(
                    crate::cstr!(
                        "slot state from %s.__reduce__ must be a dict or have an items() method, got %.200s"
                    ),
                    (*instance.class()).tp_name,
                    (*slot_state.class()).tp_name,
                );
                if !message.is_null() {
                    reduce::chain_type_error(message);
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
                let pair = PyIter_Next(iterator);
                if pair.is_null() {
                    break;
                }

                let sequence =
                    ffi_ext::PySequence_Fast(pair, crate::cstr!("items() must return pairs"));
                pair.decref();
                if sequence.is_null() {
                    result = -1;
                    break;
                }

                if ffi_ext::PySequence_Fast_GET_SIZE(sequence) != 2 {
                    sequence.decref();
                    if PyErr_Occurred().is_null() {
                        PyErr_SetString(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack"),
                        );
                    }
                    result = -1;
                    break;
                }

                let key = ffi_ext::PySequence_Fast_GET_ITEM(sequence, 0);
                let value = ffi_ext::PySequence_Fast_GET_ITEM(sequence, 1);
                let set_result = instance.set_attr(key, value);
                sequence.decref();
                if set_result < 0 {
                    result = -1;
                    break;
                }
            }

            if result == 0 && !PyErr_Occurred().is_null() {
                result = -1;
            }
            iterator.decref();
            return result;
        }

        let mut key: *mut PyObject = ptr::null_mut();
        let mut value: *mut PyObject = ptr::null_mut();
        let mut position: Py_ssize_t = 0;
        let mut result: c_int = 0;

        while PyDict_Next(slot_state, &mut position, &mut key, &mut value) != 0 {
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

        let append = instance.getattr(STATE.s_append);
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
            let item = PyIter_Next(iterator);
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

        if result == 0 && !PyErr_Occurred().is_null() {
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
            let pair = PyIter_Next(iterator);
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
                let sequence =
                    ffi_ext::PySequence_Fast(pair, crate::cstr!("cannot unpack non-sequence"));
                if sequence.is_null() {
                    pair.decref();
                    result = -1;
                    break;
                }

                let item_count = ffi_ext::PySequence_Fast_GET_SIZE(sequence);
                if item_count != 2 {
                    sequence.decref();
                    pair.decref();
                    if item_count < 2 {
                        ffi_ext::PyErr_Format(
                            PyExc_ValueError,
                            crate::cstr!("not enough values to unpack (expected 2, got %zd)"),
                            item_count,
                        );
                    } else {
                        ffi_ext::PyErr_Format(
                            PyExc_ValueError,
                            crate::cstr!("too many values to unpack (expected 2, got %zd)"),
                            item_count,
                        );
                    }
                    result = -1;
                    break;
                }

                let key = ffi_ext::PySequence_Fast_GET_ITEM(sequence, 0).newref();
                let value = ffi_ext::PySequence_Fast_GET_ITEM(sequence, 1).newref();
                sequence.decref();
                (key, value)
            };
            pair.decref();

            let set_result = PyObject_SetItem(instance, key, value);
            key.decref();
            value.decref();
            if set_result < 0 {
                result = -1;
                break;
            }
        }

        if result == 0 && !PyErr_Occurred().is_null() {
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
            if !PyErr_Occurred().is_null() {
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
