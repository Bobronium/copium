#ifndef _COPIUM_COPY_C
#define _COPIUM_COPY_C

#include "_state.c"


static PyObject* reconstruct_state(
    PyObject* new_obj, PyObject* state, PyObject* listiter, PyObject* dictiter
) {
    if (UNLIKELY(new_obj == NULL)) {
        PyErr_SetString(PyExc_SystemError, "reconstruct_state: new_obj is NULL");
        return NULL;
    }
    if (!state)
        state = Py_None;
    if (!listiter)
        listiter = Py_None;
    if (!dictiter)
        dictiter = Py_None;

    if (state != Py_None) {
        PyObject* setstate = PyObject_GetAttr(new_obj, module_state.str_setstate);
        if (setstate) {
            PyObject* r = PyObject_CallOneArg(setstate, state);
            Py_DECREF(setstate);
            if (!r)
                return NULL;
            Py_DECREF(r);
        } else {
            PyErr_Clear();
            PyObject* dict_state = NULL;
            PyObject* slot_state = NULL;

            if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
                dict_state = PyTuple_GET_ITEM(state, 0);
                slot_state = PyTuple_GET_ITEM(state, 1);

                if (slot_state && slot_state != Py_None) {
                    PyObject* it = PyObject_GetIter(slot_state);
                    if (!it)
                        return NULL;
                    PyObject* key;
                    while ((key = PyIter_Next(it)) != NULL) {
                        PyObject* value = PyObject_GetItem(slot_state, key);
                        if (!value) {
                            Py_DECREF(key);
                            Py_DECREF(it);
                            return NULL;
                        }
                        if (PyObject_SetAttr(new_obj, key, value) < 0) {
                            Py_DECREF(value);
                            Py_DECREF(key);
                            Py_DECREF(it);
                            return NULL;
                        }
                        Py_DECREF(value);
                        Py_DECREF(key);
                    }
                    Py_DECREF(it);
                    if (PyErr_Occurred())
                        return NULL;
                }
            } else {
                dict_state = state;  // treat as mapping-like
            }

            if (dict_state && dict_state != Py_None) {
                PyObject* obj_dict = PyObject_GetAttr(new_obj, module_state.str_dict);
                if (!obj_dict)
                    return NULL;
                PyObject* update = PyObject_GetAttr(obj_dict, module_state.str_update);
                Py_DECREF(obj_dict);
                if (!update)
                    return NULL;
                PyObject* r = PyObject_CallOneArg(update, dict_state);
                Py_DECREF(update);
                if (!r)
                    return NULL;
                Py_DECREF(r);
            }
        }
    }

    if (listiter != Py_None) {
        PyObject* append = PyObject_GetAttr(new_obj, module_state.str_append);
        if (!append)
            return NULL;
        PyObject* it = PyObject_GetIter(listiter);
        if (!it) {
            Py_DECREF(append);
            return NULL;
        }
        PyObject* item;
        while ((item = PyIter_Next(it)) != NULL) {
            PyObject* r = PyObject_CallOneArg(append, item);
            Py_DECREF(item);
            if (!r) {
                Py_DECREF(it);
                Py_DECREF(append);
                return NULL;
            }
            Py_DECREF(r);
        }
        Py_DECREF(it);
        Py_DECREF(append);
        if (PyErr_Occurred())
            return NULL;
    }

    if (dictiter != Py_None) {
        PyObject* it = PyObject_GetIter(dictiter);
        if (!it)
            return NULL;
        PyObject* pair;
        while ((pair = PyIter_Next(it)) != NULL) {
            if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
                Py_DECREF(pair);
                Py_DECREF(it);
                PyErr_SetString(PyExc_ValueError, "dictiter must yield (key, value) pairs");
                return NULL;
            }
            PyObject* k = PyTuple_GET_ITEM(pair, 0);
            PyObject* v = PyTuple_GET_ITEM(pair, 1);
            Py_INCREF(k);
            Py_INCREF(v);
            if (PyObject_SetItem(new_obj, k, v) < 0) {
                Py_DECREF(k);
                Py_DECREF(v);
                Py_DECREF(pair);
                Py_DECREF(it);
                return NULL;
            }
            Py_DECREF(k);
            Py_DECREF(v);
            Py_DECREF(pair);
        }
        Py_DECREF(it);
        if (PyErr_Occurred())
            return NULL;
    }

    return Py_NewRef(new_obj);
}

/* -------------------------------- copy() ---------------------------------- */

static ALWAYS_INLINE int is_empty_initializable(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyList_Type)
        return Py_SIZE(obj) == 0;
    if (tp == &PyTuple_Type)
        return Py_SIZE(obj) == 0;
    if (tp == &PyDict_Type)
        return PyDict_Size(obj) == 0;
    if (tp == &PySet_Type)
        return PySet_Size(obj) == 0;
    if (tp == &PyFrozenSet_Type)
        return PyObject_Size(obj) == 0;
    if (tp == &PyByteArray_Type)
        return PyByteArray_Size(obj) == 0;
    return 0;
}

static PyObject* make_empty_same_type(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyList_Type)
        return PyList_New(0);
    if (tp == &PyTuple_Type)
        return PyTuple_New(0);
    if (tp == &PyDict_Type)
        return PyDict_New();
    if (tp == &PySet_Type)
        return PySet_New(NULL);
    if (tp == &PyFrozenSet_Type)
        return PyFrozenSet_New(NULL);
    if (tp == &PyByteArray_Type)
        return PyByteArray_FromStringAndSize(NULL, 0);
    Py_RETURN_NONE;  // shouldn't happen; caller guards
}

static PyObject* try_stdlib_mutable_copy(PyObject* obj) {
    PyTypeObject* tp = Py_TYPE(obj);
    if (tp == &PyDict_Type || tp == &PySet_Type || tp == &PyList_Type || tp == &PyByteArray_Type) {
        PyObject* method = PyObject_GetAttrString(obj, "copy");
        if (!method) {
            PyErr_Clear();
        } else {
            PyObject* out = PyObject_CallNoArgs(method);
            Py_DECREF(method);
            if (out)
                return out;
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

#endif