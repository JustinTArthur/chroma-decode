// SPDX-License-Identifier: GPL-3.0-or-later
//
// Locates libchromadec and generates the FFI bindings.
//
// Discovery order:
//   1. CHROMADEC_LIB_DIR + CHROMADEC_INCLUDE_DIR environment variables
//      (CHROMADEC_STATIC=1 selects the static archive).
//   2. pkg-config probe for "chromadec" (also honours a Meson devenv /
//      meson-uninstalled PKG_CONFIG_PATH).

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=CHROMADEC_LIB_DIR");
    println!("cargo:rerun-if-env-changed=CHROMADEC_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=CHROMADEC_STATIC");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");

    let statik = env::var_os("CHROMADEC_STATIC").is_some_and(|v| v != "0" && !v.is_empty());

    let mut include_paths: Vec<PathBuf> = Vec::new();
    if let Ok(lib_dir) = env::var("CHROMADEC_LIB_DIR") {
        let include_dir = env::var("CHROMADEC_INCLUDE_DIR").expect(
            "CHROMADEC_INCLUDE_DIR must be set when CHROMADEC_LIB_DIR overrides pkg-config",
        );
        println!("cargo:rustc-link-search=native={lib_dir}");
        let kind = if statik { "static" } else { "dylib" };
        println!("cargo:rustc-link-lib={kind}=chromadec");
        include_paths.push(PathBuf::from(include_dir));
    } else {
        let lib = pkg_config::Config::new()
            .atleast_version("0.1.0")
            .statik(statik)
            .probe("chromadec")
            .expect(
                "libchromadec not found via pkg-config; install it, point PKG_CONFIG_PATH at \
                 a build tree's meson-uninstalled directory, or set CHROMADEC_LIB_DIR and \
                 CHROMADEC_INCLUDE_DIR",
            );
        include_paths.extend(lib.include_paths);
    }

    let mut builder = bindgen::Builder::default()
        .header_contents("wrapper.h", "#include <chromadec/chromadec.h>\n")
        .allowlist_item("(chd|CHD|CHROMADEC)_.*")
        .default_enum_style(bindgen::EnumVariation::NewType {
            is_bitfield: false,
            is_global: false,
        })
        .derive_default(true);
    for path in &include_paths {
        builder = builder.clang_arg(format!("-I{}", path.display()));
    }

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    builder
        .generate()
        .expect("bindgen failed to generate bindings for <chromadec/chromadec.h>")
        .write_to_file(&out_path)
        .expect("failed to write bindings.rs");
}
