//! Gives the headless program the project's version by the rule the window's build follows, so
//! that both programs of a release report the same version.

use std::path::PathBuf;

#[path = "../version.rs"]
mod version;

fn main() {
    let manifest = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets it"));
    let version_file = version::version_path(&manifest);
    let effective =
        version::file_version(&version_file).and_then(|file| version::effective_version(&file));
    match effective {
        Ok(effective) => println!("cargo::rustc-env=GDL_VERSION={effective}"),
        Err(message) => println!("cargo::error={message}"),
    }
}
