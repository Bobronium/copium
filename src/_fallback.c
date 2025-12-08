#ifndef _COPIUM_FALLBACK_C
#define _COPIUM_FALLBACK_C

#include "_common.h"
#include "_state.c"
#include "_memo.c"

#if PY_VERSION_HEX < PY_VERSION_3_11_HEX
    #include "frameobject.h"
#endif

static PyObject* _build_error_identifier(PyObject* exc_type, PyObject* exc_value) {
    PyObject* type_name = NULL;
    PyObject* message = NULL;
    PyObject* result = NULL;

    if (exc_type && PyType_Check(exc_type))
        type_name = PyObject_GetAttrString(exc_type, "__name__");

    if (!type_name) {
        PyErr_Clear();
        type_name = PyUnicode_FromString("Exception");
        if (!type_name)
            goto done;
    }

    if (exc_value) {
        message = PyObject_Str(exc_value);
        if (!message)
            PyErr_Clear();
    }

    if (message && PyUnicode_GET_LENGTH(message) > 0) {
        result = PyUnicode_FromFormat("%U: %U", type_name, message);
    } else {
        result = PyUnicode_FromFormat("%U: ", type_name);
    }

done:
    Py_XDECREF(type_name);
    Py_XDECREF(message);
    return result;
}

static int _error_is_ignored(PyObject* error_identifier) {
    if (!error_identifier || !module_state.ignored_errors)
        return 0;

    Py_ssize_t n = PyTuple_GET_SIZE(module_state.ignored_errors);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* suffix = PyTuple_GET_ITEM(module_state.ignored_errors, i);
        int match = PyUnicode_Tailmatch(error_identifier, suffix, 0, PY_SSIZE_T_MAX, 1);
        if (match == 1)
            return 1;
        if (match == -1)
            PyErr_Clear();
    }
    return 0;
}

static PyObject* _build_ignored_errors_repr(PyObject* error_identifier) {
    PyObject* joined = NULL;
    PyObject* result = NULL;

    if (module_state.ignored_errors_joined && error_identifier) {
        joined = PyUnicode_FromFormat(
            "%U::%U", module_state.ignored_errors_joined, error_identifier
        );
    } else if (module_state.ignored_errors_joined) {
        joined = Py_NewRef(module_state.ignored_errors_joined);
    } else if (error_identifier) {
        joined = Py_NewRef(error_identifier);
    } else {
        joined = PyUnicode_FromString("");
    }

    if (!joined)
        goto done;

    result = PyObject_Repr(joined);

done:
    Py_XDECREF(joined);
    return result;
}

/**
 * Extract the deepcopy(...) expression from a source line.
 * Returns new reference or NULL (with no exception set) on failure.
 *
 * Example: "print(copium.deepcopy(Stubborn()))" -> "copium.deepcopy(Stubborn())"
 */
static PyObject* _extract_deepcopy_expression(PyObject* line) {
    if (!line || !PyUnicode_Check(line))
        return NULL;

    const char* line_str = PyUnicode_AsUTF8(line);
    if (!line_str)
        return NULL;

    const char* deepcopy_start = strstr(line_str, "deepcopy(");
    if (!deepcopy_start)
        return NULL;

    /* Walk backwards to find the start of the expression (e.g., "copium." or "c.") */
    const char* expr_start = deepcopy_start;
    while (expr_start > line_str) {
        char c = *(expr_start - 1);
        if (c == '.' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            expr_start--;
        } else {
            break;
        }
    }

    /* Find matching closing paren */
    const char* p = deepcopy_start + 8; /* skip "deepcopy" */
    if (*p != '(')
        return NULL;
    p++;

    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(')
            depth++;
        else if (*p == ')')
            depth--;
        else if (*p == '"' || *p == '\'') {
            /* Skip string literals */
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p + 1))
                    p++;
                p++;
            }
            if (*p == quote)
                p++;
            continue;
        }
        if (depth > 0)
            p++;
    }

    if (depth != 0)
        return NULL;

    /* p now points to the closing ')' */
    Py_ssize_t len = (p - expr_start) + 1;
    return PyUnicode_FromStringAndSize(expr_start, len);
}

/**
 * Create the "with memo" version of a deepcopy expression.
 *
 * Returns new reference or NULL on failure.
 */
static PyObject* _make_expr_with_memo(PyObject* expr) {
    if (!expr || !PyUnicode_Check(expr))
        return NULL;

    Py_ssize_t len = PyUnicode_GET_LENGTH(expr);
    if (len < 2)
        return NULL;

    /* Check last character is ')' */
    Py_UCS4 last_char = PyUnicode_ReadChar(expr, len - 1);
    if (last_char != ')')
        return NULL;

    PyObject* prefix = NULL;
    PyObject* suffix = NULL;
    PyObject* result = NULL;

    /* Check for trailing comma: "...,)" */
    if (len >= 3) {
        Py_UCS4 second_last = PyUnicode_ReadChar(expr, len - 2);
        if (second_last == ',') {
            /* "deepcopy(x,)" -> "deepcopy(x, {})"
             * Take everything up to (not including) the comma, then add ", {})" */
            prefix = PyUnicode_Substring(expr, 0, len - 2);
            if (!prefix)
                goto done;
            suffix = PyUnicode_FromString(", {})");
            if (!suffix)
                goto done;
            result = PyUnicode_Concat(prefix, suffix);
            goto done;
        }
    }

    /* Normal case: "deepcopy(x)" -> "deepcopy(x, {})" */
    prefix = PyUnicode_Substring(expr, 0, len - 1);
    if (!prefix)
        goto done;
    suffix = PyUnicode_FromString(", {})");
    if (!suffix)
        goto done;
    result = PyUnicode_Concat(prefix, suffix);

done:
    Py_XDECREF(prefix);
    Py_XDECREF(suffix);
    return result;
}


static PyObject* _get_caller_frame_info(void) {
    PyObject* result = NULL;
    PyObject* linecache = NULL;
    PyObject* getline = NULL;
    PyObject* lineno_obj = NULL;
    PyObject* line = NULL;
    PyObject* stripped = NULL;
    PyFrameObject* frame = NULL;
    PyCodeObject* code = NULL;

    frame = PyEval_GetFrame();
    if (!frame)
        goto done;

    Py_INCREF(frame);

    while (frame) {
        code = PyFrame_GetCode(frame);
        if (!code) {
            PyFrameObject* back = PyFrame_GetBack(frame);
            Py_DECREF(frame);
            frame = back;
            continue;
        }

        PyObject* filename = code->co_filename;
        PyObject* name = code->co_name;

        if (filename && name) {
            int lineno = PyFrame_GetLineNumber(frame);

            linecache = PyImport_ImportModule("linecache");
            if (!linecache) {
                PyErr_Clear();
                goto done;
            }

            getline = PyObject_GetAttrString(linecache, "getline");
            if (!getline) {
                PyErr_Clear();
                goto done;
            }

            lineno_obj = PyLong_FromLong(lineno);
            if (!lineno_obj)
                goto done;

            line = PyObject_CallFunctionObjArgs(getline, filename, lineno_obj, NULL);
            if (!line) {
                PyErr_Clear();
                line = PyUnicode_FromString("");
                if (!line)
                    goto done;
            }

            stripped = PyObject_CallMethod(line, "strip", NULL);
            if (!stripped) {
                PyErr_Clear();
                stripped = PyUnicode_FromString("");
                if (!stripped)
                    goto done;
            }

            result = PyTuple_Pack(4, filename, lineno_obj, name, stripped);
            goto done;
        }

        Py_DECREF(code);
        code = NULL;

        PyFrameObject* back = PyFrame_GetBack(frame);
        Py_DECREF(frame);
        frame = back;
    }

done:
    Py_XDECREF(linecache);
    Py_XDECREF(getline);
    Py_XDECREF(lineno_obj);
    Py_XDECREF(line);
    Py_XDECREF(stripped);
    Py_XDECREF(code);
    Py_XDECREF(frame);
    return result;
}

static PyObject* _format_combined_traceback(PyObject* caller_info, PyObject* exc_value) {
    PyObject* result = NULL;
    PyObject* parts = NULL;
    PyObject* traceback_module = NULL;
    PyObject* format_exception = NULL;
    PyObject* tb_lines = NULL;
    PyObject* empty_str = NULL;
    PyObject* caller_str = NULL;

    traceback_module = PyImport_ImportModule("traceback");
    if (!traceback_module)
        goto done;

    format_exception = PyObject_GetAttrString(traceback_module, "format_exception");
    if (!format_exception)
        goto done;

    tb_lines = PyObject_CallOneArg(format_exception, exc_value);
    if (!tb_lines || !PyList_Check(tb_lines))
        goto done;

    empty_str = PyUnicode_FromString("");
    if (!empty_str)
        goto done;

    parts = PyList_New(0);
    if (!parts)
        goto done;

    if (caller_info && PyTuple_Check(caller_info) && PyTuple_GET_SIZE(caller_info) == 4) {
        PyObject* filename = PyTuple_GET_ITEM(caller_info, 0);
        PyObject* lineno = PyTuple_GET_ITEM(caller_info, 1);
        PyObject* funcname = PyTuple_GET_ITEM(caller_info, 2);
        PyObject* line = PyTuple_GET_ITEM(caller_info, 3);

        caller_str = PyUnicode_FromFormat(
            "  File \"%U\", line %S, in %U\n    %U\n", filename, lineno, funcname, line
        );
        if (!caller_str)
            PyErr_Clear();
    }

    Py_ssize_t n = PyList_GET_SIZE(tb_lines);
    int found_traceback_header = 0;
    int caller_inserted = 0;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* line = PyList_GET_ITEM(tb_lines, i);

        /* Check if this line is the traceback header */
        if (!found_traceback_header && PyUnicode_Check(line)) {
            const char* line_str = PyUnicode_AsUTF8(line);
            if (line_str && strncmp(line_str, "Traceback", 9) == 0) {
                found_traceback_header = 1;
                PyList_Append(parts, line);
                /* Insert caller frame right after the header */
                if (caller_str) {
                    PyList_Append(parts, caller_str);
                    caller_inserted = 1;
                }
                continue;
            }
        }

        PyList_Append(parts, line);
    }

    if (!found_traceback_header && caller_str && !caller_inserted) {
        PyObject* header = PyUnicode_FromString("Traceback (most recent call last):\n");
        if (header) {
            PyList_Insert(parts, 0, header);
            PyList_Insert(parts, 1, caller_str);
            Py_DECREF(header);
        }
    }

    result = PyUnicode_Join(empty_str, parts);

done:
    Py_XDECREF(parts);
    Py_XDECREF(traceback_module);
    Py_XDECREF(format_exception);
    Py_XDECREF(tb_lines);
    Py_XDECREF(empty_str);
    Py_XDECREF(caller_str);
    return result;
}

static int _emit_fallback_warning(PyObject* exc_value, PyObject* obj, PyObject* error_identifier) {
    int status = 0;
    PyObject* caller_info = NULL;
    PyObject* tb_str = NULL;
    PyObject* full_message = NULL;
    PyObject* type_obj = NULL;
    PyObject* module_name = NULL;
    PyObject* type_name = NULL;
    PyObject* deepcopy_qualname = NULL;
    PyObject* deepcopy_expr = NULL;
    PyObject* deepcopy_expr_with_memo = NULL;
    PyObject* call_site_line = NULL;
    PyObject* call_site_with_memo = NULL;

    caller_info = _get_caller_frame_info();

    tb_str = _format_combined_traceback(caller_info, exc_value);
    if (!tb_str) {
        PyErr_Clear();
        tb_str = PyUnicode_FromString("[traceback unavailable]\n");
        if (!tb_str)
            goto done;
    }

    type_obj = (PyObject*)Py_TYPE(obj);
    module_name = PyObject_GetAttrString(type_obj, "__module__");
    if (!module_name) {
        PyErr_Clear();
        module_name = PyUnicode_FromString("__main__");
        if (!module_name)
            goto done;
    }

    type_name = PyObject_GetAttrString(type_obj, "__name__");
    if (!type_name) {
        PyErr_Clear();
        type_name = PyUnicode_FromString("?");
        if (!type_name)
            goto done;
    }

    deepcopy_qualname = PyUnicode_FromFormat("%U.%U.__deepcopy__", module_name, type_name);
    if (!deepcopy_qualname)
        goto done;

    if (caller_info && PyTuple_Check(caller_info) && PyTuple_GET_SIZE(caller_info) == 4) {
        PyObject* line = PyTuple_GET_ITEM(caller_info, 3);
        deepcopy_expr = _extract_deepcopy_expression(line);
        call_site_line = Py_NewRef(line);
    }

    if (deepcopy_expr) {
        deepcopy_expr_with_memo = _make_expr_with_memo(deepcopy_expr);
        if (!deepcopy_expr_with_memo)
            PyErr_Clear();

        if (call_site_line && deepcopy_expr_with_memo) {
            const char* line_str = PyUnicode_AsUTF8(call_site_line);
            const char* expr_str = PyUnicode_AsUTF8(deepcopy_expr);
            if (line_str && expr_str) {
                const char* pos = strstr(line_str, expr_str);
                if (pos) {
                    Py_ssize_t prefix_len = pos - line_str;
                    Py_ssize_t suffix_start = prefix_len + (Py_ssize_t)strlen(expr_str);
                    PyObject* prefix_str = PyUnicode_FromStringAndSize(line_str, prefix_len);
                    PyObject* suffix_str = PyUnicode_FromString(line_str + suffix_start);
                    if (prefix_str && suffix_str) {
                        call_site_with_memo = PyUnicode_FromFormat(
                            "%U%U%U", prefix_str, deepcopy_expr_with_memo, suffix_str
                        );
                    }
                    Py_XDECREF(prefix_str);
                    Py_XDECREF(suffix_str);
                }
            }
        }
    }

    if (!deepcopy_expr) {
        deepcopy_expr = PyUnicode_FromFormat("deepcopy(%U())", type_name);
        if (!deepcopy_expr)
            goto done;
    }
    if (!deepcopy_expr_with_memo) {
        deepcopy_expr_with_memo = _make_expr_with_memo(deepcopy_expr);
        if (!deepcopy_expr_with_memo) {
            PyErr_Clear();
            deepcopy_expr_with_memo = PyUnicode_FromFormat("deepcopy(%U(), {})", type_name);
            if (!deepcopy_expr_with_memo)
                goto done;
        }
    }
    if (!call_site_line) {
        call_site_line = Py_NewRef(deepcopy_expr);
    }
    if (!call_site_with_memo) {
        call_site_with_memo = Py_NewRef(deepcopy_expr_with_memo);
    }

    full_message = PyUnicode_FromFormat(
        "\n"
        "\n"
        "Seems like 'copium.memo' was rejected inside '%U':\n"
        "\n"
        "%U"
        "\n"
        "copium was able to recover from this error, but this is slow and unreliable.\n"
        "\n"
        "Fix:\n"
        "\n"
        "  Per Python docs, '%U' should treat memo as an opaque object.\n"
        "  See: https://docs.python.org/3/library/copy.html#object.__deepcopy__\n"
        "\n"
        "Workarounds:\n"
        "\n"
        "    local  change %U to %U\n"
        "           -> copium uses dict memo in this call (recommended)\n"
        "\n"
        "   global  export COPIUM_USE_DICT_MEMO=1\n"
        "           -> copium uses dict memo everywhere (~1.3-2x slowdown, still faster than stdlib)\n"
        "\n"
        "   silent  export COPIUM_NO_MEMO_FALLBACK_WARNING='%U'\n"
        "           -> '%U' stays slow to deepcopy\n"
        "\n"
        "explosive  export COPIUM_NO_MEMO_FALLBACK=1\n"
        "           -> '%U' raises the error above\n",
        deepcopy_qualname,
        tb_str,
        deepcopy_qualname,
        deepcopy_expr,
        deepcopy_expr_with_memo,
        error_identifier,
        deepcopy_expr,
        deepcopy_expr
    );
    if (!full_message)
        goto done;

    if (PyErr_WarnEx(PyExc_UserWarning, PyUnicode_AsUTF8(full_message), 1) < 0) {
        status = -1;
        goto done;
    }

    PyErr_Clear();

done:
    Py_XDECREF(caller_info);
    Py_XDECREF(tb_str);
    Py_XDECREF(full_message);
    Py_XDECREF(module_name);
    Py_XDECREF(type_name);
    Py_XDECREF(deepcopy_qualname);
    Py_XDECREF(deepcopy_expr);
    Py_XDECREF(deepcopy_expr_with_memo);
    Py_XDECREF(call_site_line);
    Py_XDECREF(call_site_with_memo);
    return status;
}

static PyObject* _maybe_retry_with_dict_memo(
    PyObject* obj, PyObject* __deepcopy__, PyMemoObject* memo, MemoCheckpoint checkpoint
) {
    PyObject* res = NULL;
    PyObject* dict_memo = NULL;
    PyObject* exc_type = NULL;
    PyObject* exc_value = NULL;
    PyObject* exc_tb = NULL;
    PyObject* error_identifier = NULL;

    if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_AssertionError)) {
        return NULL;
    }

    if (module_state.no_memo_fallback) {
        return NULL;
    }

    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
    PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);

    memo_rollback(memo, checkpoint);

    dict_memo = memo_to_dict(memo);
    if (!dict_memo)
        goto done;

    Py_ssize_t dict_size_before = PyDict_Size(dict_memo);

    res = PyObject_CallOneArg(__deepcopy__, dict_memo);
    if (!res)
        goto done;

    if (memo_sync_from_dict(memo, dict_memo, dict_size_before) < 0)
        goto error;

    error_identifier = _build_error_identifier(exc_type, exc_value);
    if (error_identifier && !_error_is_ignored(error_identifier)) {
        /* In this branch priority is not speed, but informativeness */
        if (exc_tb && exc_value) {
            PyException_SetTraceback(exc_value, exc_tb);
        }

        if (_emit_fallback_warning(exc_value, obj, error_identifier) < 0)
            goto error;
    }

    goto done;

error:
    Py_CLEAR(res);
done:
    Py_XDECREF(dict_memo);
    Py_XDECREF(exc_type);
    Py_XDECREF(exc_value);
    Py_XDECREF(exc_tb);
    Py_XDECREF(error_identifier);
    return res;
}
#endif  // _COPIUM_FALLBACK_C
