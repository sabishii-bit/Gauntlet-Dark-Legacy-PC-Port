//! Gives the window the project's version from `VERSION`, its single source, and lets Tauri embed
//! the front end and build the Windows resources.

use std::fs::{self, File};
use std::path::{Path, PathBuf};

#[path = "../version.rs"]
mod version;

fn main() {
    let manifest = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets it"));
    let version_file = version::version_path(&manifest);
    // Tauri's build watches its configuration but not the icons it builds into the program.
    let icons = manifest.join("icons");
    println!("cargo::rerun-if-changed={}", icons.display());
    let effective = version::file_version(&version_file).and_then(|file| {
        let generated = manifest.join("gen").join("version.json");
        write_version(&generated, &file, &version_file);
        version::effective_version(&file)
    });
    match effective {
        Ok(effective) => {
            println!("cargo::rustc-env=GDL_VERSION={effective}");
            tauri_build::build();
        }
        Err(message) => println!("cargo::error={message}"),
    }
}

/// Writes the `gen/version.json` that `tauri.conf.json` names as its version; Tauri writes that
/// version into the Windows resource and the package information. A deleted or edited file
/// brings this script back to write it again.
fn write_version(generated: &Path, version: &str, version_file: &Path) {
    println!("cargo::rerun-if-changed={}", generated.display());
    let text = format!("{}\n", serde_json::json!({ "version": version }));
    if fs::read_to_string(generated).ok().as_deref() == Some(text.as_str()) {
        return;
    }
    // Written aside and renamed into place, so a build reading it never meets half a file.
    let folder = generated.parent().expect("gen/version.json has a folder");
    fs::create_dir_all(folder).expect("gen/ is writable");
    let aside = folder.join(format!("version.json.{}", std::process::id()));
    fs::write(&aside, &text).expect("gen/ is writable");
    // Cargo runs a build script again when a file it watches is newer than the script's last
    // run, so the file takes VERSION's time; failing that, the next build runs this once more.
    if let Err(error) = backdate(&aside, version_file) {
        println!("cargo::warning=gen/version.json keeps the time it was written: {error}");
    }
    if let Err(error) = fs::rename(&aside, generated) {
        let _ = fs::remove_file(&aside);
        panic!("gen/version.json cannot be replaced: {error}");
    }
}

/// Gives `path` the modification time of `like`.
fn backdate(path: &Path, like: &Path) -> std::io::Result<()> {
    let time = fs::metadata(like)?.modified()?;
    File::options().write(true).open(path)?.set_modified(time)
}
