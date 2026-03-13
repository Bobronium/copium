fn main() {
    let hash = std::env::var("COPIUM_BUILD_HASH").unwrap_or_else(|_| "dev".into());
    println!("cargo:rustc-env=COPIUM_BUILD_HASH={hash}");
    pyo3_build_config::use_pyo3_cfgs();
    emit_python_link_alias_for_custom_ffi_blocks();
    pyo3_build_config::add_extension_module_link_args();
}

fn emit_python_link_alias_for_custom_ffi_blocks() {
    let is_windows_target = std::env::var("CARGO_CFG_TARGET_OS")
        .is_ok_and(|target_operating_system| target_operating_system == "windows");

    if !is_windows_target {
        return;
    }

    let interpreter_config = pyo3_build_config::get();
    let Some(library_name) = interpreter_config.lib_name.as_ref() else {
        return;
    };

    let link_modifier = if interpreter_config.shared { "" } else { "static=" };
    println!("cargo:rustc-link-lib={link_modifier}pythonXY:{library_name}");

    if let Some(library_directory) = interpreter_config.lib_dir.as_ref() {
        println!("cargo:rustc-link-search=native={library_directory}");
    }
}
