//! The list of what an install copied from the disc, `.gdl-launcher/disc-files.json`: every folder
//! and file of the disc's `Gauntlet` and `carddemo` trees, in the disc's file table's order and
//! spelled as the disc spells them, each file with its size and SHA-256 digest, so that the tree
//! can be checked, and made again, without the disc. The empty folders are listed too, since they
//! are part of the tree the game finds.

use std::path::Path;

use gdl_files::{FilesError, MAX_JSON_LEN, write_json_atomic};
use gdl_media::Sha256Digest;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

use crate::compat::MIN_LAUNCHER;

/// The list of what an install copied from the disc, in the state folder.
pub const DISC_FILES: &str = "disc-files.json";

/// The schema of the disc's lists this launcher reads and writes.
pub const DISC_FILES_SCHEMA: u64 = 1;

/// What an install copied from the disc. The keys a list holds beyond these, at its top and in its
/// rows, are kept, so that what a later launcher adds survives this one's rewrite.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DiscFiles {
    /// The list's schema: [`DISC_FILES_SCHEMA`] in one this launcher writes.
    pub schema: u64,
    /// The lowest launcher version allowed to change the list: [`MIN_LAUNCHER`] in one this
    /// launcher writes.
    pub min_launcher: String,
    /// Every folder copied, by its path in the disc's tree, in the file table's order.
    pub folders: Vec<String>,
    /// Every file copied, in the file table's order.
    pub files: Vec<CopiedFile>,
    /// The keys the list holds beyond these, kept as they are.
    #[serde(flatten)]
    pub other: Map<String, Value>,
}

/// A file the install copied from the disc: its path as the disc spells it, its size and its
/// digest.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct CopiedFile {
    /// The file's path in the disc's tree, its names joined by `/`.
    pub path: String,
    /// How many bytes it holds.
    pub size: u64,
    /// Its SHA-256 digest, in lower-case hexadecimal.
    pub sha256: String,
    /// The keys the row holds beyond these, kept as they are.
    #[serde(flatten)]
    pub other: Map<String, Value>,
}

impl DiscFiles {
    /// The list of `folders` and `files`, stamped as this launcher writes one.
    pub(crate) fn listing(folders: Vec<String>, files: Vec<CopiedFile>) -> DiscFiles {
        DiscFiles {
            schema: DISC_FILES_SCHEMA,
            min_launcher: MIN_LAUNCHER.to_owned(),
            folders,
            files,
            other: Map::new(),
        }
    }

    /// Writes the list whole into the state folder `state`, to the bound the launcher reads its
    /// JSON state files to. The caller holds the installation's lock.
    pub(crate) fn write(&self, state: &Path) -> Result<(), FilesError> {
        write_json_atomic(&state.join(DISC_FILES), self, MAX_JSON_LEN)
    }
}

impl CopiedFile {
    /// The row of the file at `path` in the disc's tree, of `size` bytes whose digest is `sha256`.
    pub(crate) fn new(path: &str, size: u64, sha256: Sha256Digest) -> CopiedFile {
        CopiedFile {
            path: path.to_owned(),
            size,
            sha256: sha256.hex(),
            other: Map::new(),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::fs;

    use gdl_files::{MAX_DISC_PATH_CHARS, MAX_JSON_LEN};
    use gdl_media::{MAX_DISC_ENTRIES, Sha256Digest};
    use serde_json::json;

    use super::{CopiedFile, DISC_FILES, DiscFiles};

    /// A list of one folder and one empty file, as the install writes it.
    const ONE_FILE: &str = r#"{
  "schema": 1,
  "minLauncher": "1.0.0-beta",
  "folders": [
    "carddemo"
  ],
  "files": [
    {
      "path": "carddemo/icon.tpl",
      "size": 0,
      "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    }
  ]
}
"#;

    #[test]
    fn a_list_is_written_as_its_format_gives_it() {
        let state = tempfile::tempdir().unwrap();
        let file = CopiedFile::new("carddemo/icon.tpl", 0, Sha256Digest::of(b""));
        DiscFiles::listing(vec!["carddemo".to_owned()], vec![file])
            .write(state.path())
            .unwrap();
        assert_eq!(
            fs::read_to_string(state.path().join(DISC_FILES)).unwrap(),
            ONE_FILE
        );
    }

    #[test]
    fn a_list_keeps_what_it_does_not_know() {
        let text = json!({
            "schema": 1,
            "minLauncher": "1.0.0-beta",
            "folders": ["Gauntlet"],
            "files": [{"path": "Gauntlet/A", "size": 1, "sha256": "0".repeat(64), "mode": 7}],
            "future": [1],
        });
        let list: DiscFiles = serde_json::from_value(text.clone()).unwrap();
        assert_eq!(list.other["future"], json!([1]));
        assert_eq!(list.files[0].other["mode"], 7);
        assert_eq!(serde_json::to_value(&list).unwrap(), text);
    }

    #[test]
    fn the_longest_list_a_disc_may_give_fits_its_bound() {
        // As many files as a disc may keep, each of the longest path and size it may have.
        let path = "a".repeat(MAX_DISC_PATH_CHARS);
        let file = CopiedFile::new(&path, u64::from(u32::MAX), Sha256Digest::of(b""));
        let list = DiscFiles::listing(Vec::new(), vec![file; MAX_DISC_ENTRIES]);
        let state = tempfile::tempdir().unwrap();
        list.write(state.path()).unwrap();
        let written = fs::metadata(state.path().join(DISC_FILES)).unwrap().len();
        assert!(written <= MAX_JSON_LEN, "{written}");
    }
}
