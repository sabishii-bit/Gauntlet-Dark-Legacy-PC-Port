//! Gives the window the project's version from `VERSION`, its single source, and lets Tauri embed
//! the front end and build the Windows resources.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets it"));
    let version_file = manifest.join("../../../VERSION");
    println!("cargo::rerun-if-changed={}", version_file.display());
    // Tauri's build watches its configuration but not the icons it builds into the program.
    let icons = manifest.join("icons");
    println!("cargo::rerun-if-changed={}", icons.display());
    let version = std::fs::read_to_string(&version_file).expect("VERSION is readable");
    println!("cargo::rustc-env=GDL_VERSION={}", version.trim());
    tauri_build::build();
}
