use std::ffi::CString;
use std::ptr;

use crate::py::{self, *};

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
        module.add_module_string_constant(crate::cstr!("__version__"), version_cstring.as_c_str());

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

        let vi_cls = py::call::call_function!(
            namedtuple,
            crate::cstr!("s[ssssss]"),
            crate::cstr!("VersionInfo"),
            crate::cstr!("major"),
            crate::cstr!("minor"),
            crate::cstr!("patch"),
            crate::cstr!("pre"),
            crate::cstr!("dev"),
            crate::cstr!("local"),
        );
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

        let vi = py::call::call_function!(
            vi_cls,
            crate::cstr!("lllOOs"),
            major,
            minor,
            patch,
            py::NoneObject,
            py::NoneObject,
            local_cstring.as_c_str(),
        );

        module.add_module_object(crate::cstr!("VersionInfo"), vi_cls);
        if !vi.is_null() {
            module.add_module_object(crate::cstr!("__version_tuple__"), vi);
        }

        module.add_module_object(crate::cstr!("__commit_id__"), py::NoneObject.newref());

        module.add_module_string_constant(crate::cstr!("__build_hash__"), local_cstring.as_c_str());

        let author_cls = py::call::call_function!(
            namedtuple,
            crate::cstr!("s[ss]"),
            crate::cstr!("Author"),
            crate::cstr!("name"),
            crate::cstr!("email"),
        );
        namedtuple.decref();
        if author_cls.is_null() {
            module.decref();
            return -1;
        }

        let author = py::call::call_function!(
            author_cls,
            crate::cstr!("ss"),
            crate::cstr!("Arseny Boykov (Bobronium)"),
            crate::cstr!("hi@bobronium.me"),
        );
        module.add_module_object(crate::cstr!("Author"), author_cls);

        if !author.is_null() {
            let authors = py::tuple::new(1);
            if authors.is_null() {
                module.decref();
                return -1;
            }
            authors.steal_item_unchecked(0, author);
            module.add_module_object(crate::cstr!("__authors__"), authors);
        }

        crate::add_submodule(parent, crate::cstr!("__about__"), module)
    }
}
