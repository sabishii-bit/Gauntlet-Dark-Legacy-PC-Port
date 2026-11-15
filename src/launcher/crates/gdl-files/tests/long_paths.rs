//! The launcher's file operations under the longest install root it allows on Windows, where what
//! it stages lies deeper than MAX_PATH. This test program, like the launcher, is not long-path
//! aware, so every call must reach past MAX_PATH by itself.

use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use gdl_files::{
    FilesError, Journal, MAX_INSTALL_ROOT_UTF16, MAX_JSON_LEN, PAYLOAD_NAMES, RelPath, Staging,
    no_links, read_json_bounded, rename_noreplace, replace, root_fits, root_utf16_len,
    write_json_atomic,
};
use serde_json::{Value, json};

/// MAX_PATH: the most UTF-16 units a plain Windows path may hold, its NUL included.
const MAX_PATH: usize = 260;

/// How long the path of the deepest file an expansion stages is under the longest root: the
/// root, a separator and the 111 units below it.
const DEEPEST_STAGED_UNITS: usize = 292;

/// The folder of a file staged at least that deep under a staging named for the N64 expansion.
const DEEP_FOLDER: &str = "n64/extracted/native/levels/levelt1/rombackup_3_20_00";

/// That file's name.
const DEEP_NAME: &str = "textures-and-palettes.rom";

/// A journal that writes down the roots it is told of.
#[derive(Default)]
struct Recorder(Vec<String>);

impl Journal for Recorder {
    fn published(&mut self, root: &RelPath) -> Result<(), FilesError> {
        self.0.push(root.as_str().to_owned());
        Ok(())
    }
}

fn rel(text: &str) -> RelPath {
    RelPath::parse(text, &PAYLOAD_NAMES).unwrap_or_else(|error| panic!("{error}"))
}

/// Makes the file `path` in `staging` as a job does, new, written, synced and closed, and returns
/// where it is.
fn stage(staging: &Staging, path: &RelPath, bytes: &[u8]) -> PathBuf {
    let mut file = staging.create_file(path).unwrap();
    file.write_all(bytes).unwrap();
    file.sync_all().unwrap();
    path.path_under(staging.path())
}

/// The names in `folder`, sorted.
fn names(folder: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(folder)
        .unwrap()
        .map(|entry| entry.unwrap().file_name().into_string().unwrap())
        .collect();
    names.sort();
    names
}

#[test]
fn everything_works_under_a_180_unit_root() {
    let base = tempfile::tempdir().unwrap();
    let root = gdl_fixtures::long_root(base.path(), MAX_INSTALL_ROOT_UTF16).unwrap();
    assert_eq!(MAX_INSTALL_ROOT_UTF16, 180);
    assert_eq!(root_utf16_len(&root).unwrap(), MAX_INSTALL_ROOT_UTF16);
    assert!(root_fits(&root).unwrap());
    let state = root.join(".gdl-launcher");
    fs::create_dir(&state).unwrap();
    let staging = Staging::create_in(&state, "stage-n64-").unwrap();

    // A file past MAX_PATH, made new in the staging with every folder above it.
    let deep = rel(&format!("{DEEP_FOLDER}/{DEEP_NAME}"));
    let file = stage(&staging, &deep, b"textures");
    let units = root_utf16_len(&file).unwrap();
    assert!(units > MAX_PATH, "{units}");
    assert!(units >= DEEPEST_STAGED_UNITS, "{units}");
    assert_eq!(fs::read(&file).unwrap(), b"textures");
    no_links(&file).unwrap();

    // Renamed there, never over what is there: the file, the folder that holds it, itself past
    // MAX_PATH, and the staged root that holds them all.
    let folder = file.parent().unwrap().to_path_buf();
    assert!(root_utf16_len(&folder).unwrap() > MAX_PATH);
    let aside = folder.join("textures-and-palettes.bak");
    rename_noreplace(&file, &aside).unwrap();
    stage(&staging, &deep, b"newer");
    let error = rename_noreplace(&aside, &file).unwrap_err();
    assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{error}");
    assert_eq!(fs::read(&file).unwrap(), b"newer");
    fs::remove_file(&file).unwrap();
    rename_noreplace(&aside, &file).unwrap();
    for from in [folder.clone(), staging.path().join("n64")] {
        let moved = from.with_file_name("moved");
        rename_noreplace(&from, &moved).unwrap();
        assert!(fs::symlink_metadata(&from).is_err());
        rename_noreplace(&moved, &from).unwrap();
    }
    assert_eq!(fs::read(&file).unwrap(), b"textures");

    // A state file there, written whole, read back, then replaced by a file staged beside it.
    let manifest = folder.join("manifest.json");
    let written = json!({"schema": 1, "files": [deep.as_str()]});
    write_json_atomic(&manifest, &written).unwrap();
    let read: Value = read_json_bounded(&manifest, MAX_JSON_LEN).unwrap();
    assert_eq!(read, written);
    let next = stage(
        &staging,
        &rel(&format!("{DEEP_FOLDER}/manifest.json.next")),
        b"{}\n",
    );
    replace(&next, &manifest).unwrap();
    assert_eq!(fs::read(&manifest).unwrap(), b"{}\n");
    assert_eq!(names(&folder), ["manifest.json", DEEP_NAME]);

    // A root as deep that is never published.
    let unpublished = DEEP_FOLDER.replacen("n64", "n64-unpublished", 1);
    let left = stage(
        &staging,
        &rel(&format!("{unpublished}/{DEEP_NAME}")),
        b"left",
    );
    assert!(root_utf16_len(&left).unwrap() > MAX_PATH);

    // Published into the root by rename, then dropped with what it still holds.
    let mut journal = Recorder::default();
    staging
        .publish_new(&[rel("n64")], &root, &mut journal)
        .unwrap();
    assert_eq!(journal.0, ["n64"]);
    let published = deep.path_under(&root);
    assert_eq!(fs::read(&published).unwrap(), b"textures");
    no_links(&published).unwrap();
    let manifest = published.with_file_name("manifest.json");
    assert_eq!(fs::read(manifest).unwrap(), b"{}\n");
    let staged = staging.path().to_path_buf();
    assert_eq!(names(&staged), ["n64-unpublished"]);
    drop(staging);
    assert!(fs::symlink_metadata(&staged).is_err());
    assert!(names(&state).is_empty());
    assert_eq!(names(&root), [".gdl-launcher", "n64"]);
}
