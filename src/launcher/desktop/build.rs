//! Gives the window the project's version from `VERSION`, its single source, and lets Tauri embed
//! the front end and build the Windows resources.

use std::fs::{self, File};
use std::path::{Path, PathBuf};

use serde_json::Value;

fn main() {
    let manifest = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets it"));
    let version_file = manifest.join("../../../VERSION");
    println!("cargo::rerun-if-changed={}", version_file.display());
    // Tauri's build watches its configuration but not the icons it builds into the program.
    let icons = manifest.join("icons");
    println!("cargo::rerun-if-changed={}", icons.display());
    let version = fs::read_to_string(&version_file).expect("VERSION is readable");
    let version = version.trim();
    let generated = manifest.join("gen").join("version.json");
    write_version(&generated, version, &version_file);
    println!("cargo::rerun-if-env-changed=TAURI_CONFIG");
    match overriding_version() {
        Ok(overriding) => {
            let version = overriding.as_deref().unwrap_or(version);
            println!("cargo::rustc-env=GDL_VERSION={version}");
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

/// The version that `TAURI_CONFIG`, Tauri's own override, gives the program, if it gives one.
/// Tauri then writes it into the resource and the package information in place of VERSION's.
fn overriding_version() -> Result<Option<String>, String> {
    let Ok(patch) = std::env::var("TAURI_CONFIG") else {
        return Ok(None);
    };
    let patch: Value = serde_json::from_str(&patch)
        .map_err(|error| format!("TAURI_CONFIG is not JSON: {error}"))?;
    match patch.get("version") {
        None => Ok(None),
        // Tauri reads a version that names a file as that file's path, so only a version proper
        // may stand for the program's.
        Some(Value::String(version)) if semver::Version::parse(version).is_ok() => {
            Ok(Some(version.clone()))
        }
        Some(other) => Err(format!("TAURI_CONFIG has version {other}, not a version")),
    }
}
