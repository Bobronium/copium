use pyo3_ffi::*;
use std::ffi::CString;
use std::ptr;

use crate::py;
use crate::types::{PyObjectPtr, PySeqPtr};

static mut ABOUT_METHODS: [PyMethodDef; 1] = [PyMethodDef::zeroed()];

static mut ABOUT_MODULE_DEF: PyModuleDef = PyModuleDef {
    m_base: PyModuleDef_HEAD_INIT,
    m_name: b"copium.__about__\0".as_ptr().cast(),
    m_doc: b"Version information for copium.\0".as_ptr().cast(),
    m_size: -1,
    m_methods: ptr::null_mut(),
    m_slots: ptr::null_mut(),
    m_traverse: None,
    m_clear: None,
    m_free: None,
};

pub unsafe fn create_module(parent: *mut PyObject) -> i32 {
    unsafe {
        ABOUT_MODULE_DEF.m_methods = ptr::addr_of_mut!(ABOUT_METHODS).cast::<PyMethodDef>();

        let module = py::module::create(std::ptr::addr_of_mut!(ABOUT_MODULE_DEF));
        if module.is_null() {
            return -1;
        }

        let version = env!("CARGO_PKG_VERSION");
        let version_cstring = CString::new(version).unwrap();
        py::module::add_string_constant(
            module,
            crate::cstr!("__version__"),
            version_cstring.as_c_str(),
        );

        let collections = py::module::import(crate::cstr!("collections"));
        if collections.is_null() {
            module.decref();
            return -1;
        }
        let namedtuple = collections.getattr_cstr(crate::cstr!("namedtuple"));
        collections.decref();
        if namedtuple.is_null() {
            module.decref();
            return -1;
        }

        let version_info_name = py::unicode::from_cstr(crate::cstr!("VersionInfo"));
        let version_info_fields = py::list::new(6);
        if version_info_name.is_null() || version_info_fields.is_null() {
            version_info_name.decref_if_nonnull();
            version_info_fields.decref_if_nonnull();
            namedtuple.decref();
            module.decref();
            return -1;
        }
        version_info_fields.steal_item_unchecked(0, py::unicode::from_cstr(crate::cstr!("major")).as_object());
        version_info_fields.steal_item_unchecked(1, py::unicode::from_cstr(crate::cstr!("minor")).as_object());
        version_info_fields.steal_item_unchecked(2, py::unicode::from_cstr(crate::cstr!("patch")).as_object());
        version_info_fields.steal_item_unchecked(3, py::unicode::from_cstr(crate::cstr!("pre")).as_object());
        version_info_fields.steal_item_unchecked(4, py::unicode::from_cstr(crate::cstr!("dev")).as_object());
        version_info_fields.steal_item_unchecked(5, py::unicode::from_cstr(crate::cstr!("local")).as_object());

        let version_info_args = py::tuple::new(2);
        if version_info_args.is_null() {
            version_info_name.decref();
            version_info_fields.decref();
            namedtuple.decref();
            module.decref();
            return -1;
        }
        version_info_args.steal_item_unchecked(0, version_info_name.as_object());
        version_info_args.steal_item_unchecked(1, version_info_fields.as_object());

        let vi_cls = namedtuple.call_with(version_info_args);
        version_info_args.decref();
        if vi_cls.is_null() {
            namedtuple.decref();
            module.decref();
            return -1;
        }

        // Parse version
        let parts: Vec<&str> = version.split('.').collect();
        let major = parts
            .first()
            .and_then(|s| s.parse::<i64>().ok())
            .unwrap_or(0);
        let minor = parts
            .get(1)
            .and_then(|s| s.parse::<i64>().ok())
            .unwrap_or(0);
        let patch_str = parts.get(2).map(|s| *s).unwrap_or("0");
        let patch = patch_str
            .split('+')
            .next()
            .and_then(|s| s.parse::<i64>().ok())
            .unwrap_or(0);

        let build_hash = env!("COPIUM_BUILD_HASH");
        let local_cstring = CString::new(build_hash).unwrap();

        let version_info_value_args = py::tuple::new(6);
        if version_info_value_args.is_null() {
            namedtuple.decref();
            vi_cls.decref();
            module.decref();
            return -1;
        }
        version_info_value_args.steal_item_unchecked(0, py::long::from_i64(major).as_object());
        version_info_value_args.steal_item_unchecked(1, py::long::from_i64(minor).as_object());
        version_info_value_args.steal_item_unchecked(2, py::long::from_i64(patch).as_object());
        version_info_value_args.steal_item_unchecked(3, py::none().newref());
        version_info_value_args.steal_item_unchecked(4, py::none().newref());
        version_info_value_args.steal_item_unchecked(
            5,
            py::unicode::from_cstr(local_cstring.as_c_str()).as_object(),
        );

        let vi = vi_cls.call_with(version_info_value_args);
        version_info_value_args.decref();

        py::module::add_object(module, crate::cstr!("VersionInfo"), vi_cls);
        if !vi.is_null() {
            py::module::add_object(module, crate::cstr!("__version_tuple__"), vi);
        }

        py::module::add_object(module, crate::cstr!("__commit_id__"), py::none().newref());

        py::module::add_string_constant(
            module,
            crate::cstr!("__build_hash__"),
            local_cstring.as_c_str(),
        );

        let author_name = py::unicode::from_cstr(crate::cstr!("Author"));
        let author_fields = py::list::new(2);
        if author_name.is_null() || author_fields.is_null() {
            author_name.decref_if_nonnull();
            author_fields.decref_if_nonnull();
            namedtuple.decref();
            vi_cls.decref();
            module.decref();
            return -1;
        }
        author_fields.steal_item_unchecked(0, py::unicode::from_cstr(crate::cstr!("name")).as_object());
        author_fields.steal_item_unchecked(1, py::unicode::from_cstr(crate::cstr!("email")).as_object());

        let author_type_args = py::tuple::new(2);
        if author_type_args.is_null() {
            author_name.decref();
            author_fields.decref();
            namedtuple.decref();
            vi_cls.decref();
            module.decref();
            return -1;
        }
        author_type_args.steal_item_unchecked(0, author_name.as_object());
        author_type_args.steal_item_unchecked(1, author_fields.as_object());

        let author_cls = namedtuple.call_with(author_type_args);
        author_type_args.decref();
        namedtuple.decref();
        if author_cls.is_null() {
            module.decref();
            return -1;
        }

        let author_value_args = py::tuple::new(2);
        if author_value_args.is_null() {
            author_cls.decref();
            module.decref();
            return -1;
        }
        author_value_args.steal_item_unchecked(
            0,
            py::unicode::from_cstr(crate::cstr!("Arseny Boykov (Bobronium)")).as_object(),
        );
        author_value_args.steal_item_unchecked(
            1,
            py::unicode::from_cstr(crate::cstr!("hi@bobronium.me")).as_object(),
        );
        let author = author_cls.call_with(author_value_args);
        author_value_args.decref();
        py::module::add_object(module, crate::cstr!("Author"), author_cls);

        if !author.is_null() {
            let authors = py::tuple::new(1);
            if authors.is_null() {
                module.decref();
                return -1;
            }
            authors.steal_item_unchecked(0, author);
            py::module::add_object(module, crate::cstr!("__authors__"), authors);
        }

        crate::add_submodule(parent, crate::cstr!("__about__"), module)
    }
}
