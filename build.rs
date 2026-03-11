fn main() {
    let hash = std::env::var("COPIUM_BUILD_HASH").unwrap_or_else(|_| "dev".into());
    println!("cargo:rustc-env=COPIUM_BUILD_HASH={hash}");
    pyo3_build_config::use_pyo3_cfgs();
    pyo3_build_config::add_extension_module_link_args();
}