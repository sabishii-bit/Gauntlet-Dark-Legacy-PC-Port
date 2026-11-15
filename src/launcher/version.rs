//! The version the launcher's programs carry, for their build scripts to include: `VERSION`, or
//! the version `TAURI_CONFIG` gives in its place. One rule for every program, so the window and
//! the headless program never report different versions of one release.

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

/// The repository's `VERSION`, the version's single source, seen from a launcher crate's manifest
/// folder. Cargo runs the build script again when it changes.
pub fn version_path(manifest_dir: &Path) -> PathBuf {
    let path = manifest_dir.join("../../../VERSION");
    println!("cargo::rerun-if-changed={}", path.display());
    path
}

/// The version `VERSION` holds, without the whitespace around it: a checkout may end its line
/// with a carriage return as well as a line feed.
pub fn file_version(path: &Path) -> Result<String, String> {
    let contents =
        fs::read_to_string(path).map_err(|error| format!("VERSION cannot be read: {error}"))?;
    let text = contents.trim_ascii();
    if semver::Version::parse(text).is_err() {
        return Err(format!("VERSION is not a semantic version: {text:?}"));
    }
    Ok(text.to_owned())
}

/// The version a program carries: `file`, unless `TAURI_CONFIG`, Tauri's own override, gives one,
/// which Tauri then writes into the window's resource and package information instead.
pub fn effective_version(file: &str) -> Result<String, String> {
    println!("cargo::rerun-if-env-changed=TAURI_CONFIG");
    let Ok(patch) = std::env::var("TAURI_CONFIG") else {
        return Ok(file.to_owned());
    };
    let patch: Value = serde_json::from_str(&patch)
        .map_err(|error| format!("TAURI_CONFIG is not JSON: {error}"))?;
    match patch.get("version") {
        None => Ok(file.to_owned()),
        // Tauri reads a version that names a file as that file's path, so only a version proper
        // may stand for the program's.
        Some(Value::String(version)) if semver::Version::parse(version).is_ok() => {
            Ok(version.clone())
        }
        Some(other) => Err(format!("TAURI_CONFIG has version {other}, not a version")),
    }
}
