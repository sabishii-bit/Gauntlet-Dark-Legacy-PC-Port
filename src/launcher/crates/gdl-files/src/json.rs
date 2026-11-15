//! The launcher's JSON state files: read to a bound, so that a damaged or hostile file cannot
//! make the launcher read without end or wait for a writer, and written whole, so that a reader,
//! or the next start after a crash, finds either the old file or the new one and never part of
//! either.

use std::fs::{self, File, OpenOptions};
use std::io::{self, BufWriter, Read, Write};
use std::path::{Path, PathBuf};

use serde::Serialize;
use serde::de::DeserializeOwned;

use crate::rename::{folder_of, replace};
use crate::{FilesError, no_links};

/// The most bytes a JSON file the launcher reads may hold, unless its kind holds it to less.
pub const MAX_JSON_LEN: u64 = 4 << 20;

/// What a JSON state file's name takes while its new contents are written: the name of a file of
/// the launcher's own, which only it makes and only it removes.
pub const PENDING_SUFFIX: &str = ".pending";

/// The file that the new contents of the state file `path` are written to before they replace
/// it: its name with [`PENDING_SUFFIX`] after it, in the same folder. A path that names no file,
/// which [`write_json_atomic`] refuses, gets the suffix alone.
pub fn pending_path(path: &Path) -> PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_owned();
    name.push(PENDING_SUFFIX);
    path.with_file_name(name)
}

/// Reads the whole file at `path`, refusing it when it, or a folder above it, is a link, when it
/// is not a regular file, and when it holds more than `limit` bytes; a file of exactly `limit`
/// bytes is read. Whatever is at `path`, the read never waits for a writer and takes no more than
/// one byte past `limit`.
pub fn read_bounded(path: &Path, limit: u64) -> Result<Vec<u8>, FilesError> {
    no_links(path)?;
    let file = open_regular(path).map_err(|source| FilesError::io(path, source))?;
    match read_to_bound(file, limit) {
        Ok(Some(bytes)) => Ok(bytes),
        Ok(None) => Err(FilesError::TooLarge {
            path: path.to_path_buf(),
            limit,
        }),
        Err(source) => Err(FilesError::io(path, source)),
    }
}

/// Opens the file at `path` to read, and refuses it unless it is a regular file: a FIFO, a socket,
/// a device or a folder holds no state of the launcher's. On Unix the open itself never waits, as
/// a plain open of a FIFO waits for a writer, and never follows a link that took the name since
/// [`no_links`] looked.
fn open_regular(path: &Path) -> io::Result<File> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW);
    }
    let file = options.open(path)?;
    if file.metadata()?.is_file() {
        Ok(file)
    } else {
        Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "not a regular file",
        ))
    }
}

/// Reads all of `source` when it holds at most `limit` bytes, and gives `None` when it holds
/// more, taking no more than one byte past `limit` from it either way.
fn read_to_bound(source: impl Read, limit: u64) -> io::Result<Option<Vec<u8>>> {
    let mut bytes = Vec::new();
    source
        .take(limit.saturating_add(1))
        .read_to_end(&mut bytes)?;
    let within = u64::try_from(bytes.len()).is_ok_and(|read| read <= limit);
    Ok(within.then_some(bytes))
}

/// Reads the file at `path` as [`read_bounded`] does, and then as JSON into a `T`.
pub fn read_json_bounded<T: DeserializeOwned>(path: &Path, limit: u64) -> Result<T, FilesError> {
    let bytes = read_bounded(path, limit)?;
    serde_json::from_slice(&bytes).map_err(|source| FilesError::Json {
        path: path.to_path_buf(),
        source,
    })
}

/// Writes `value` as the JSON state file `path`, whole: the JSON, indented by two spaces and
/// ending in a newline, with LF line ends on every system, goes into the file [`pending_path`]
/// names, made new, synced and closed, which then replaces `path` in one rename that is itself on
/// disk before this returns. The caller holds the lock of the folder, which must have no link on
/// its path.
///
/// A pending file that an interrupted write left behind is removed first, a link as a link and
/// never what it leads to; a folder of that name is not the launcher's to empty, and refuses the
/// write. Once the pending file is made, a failure removes it again where it can. An error in
/// putting the rename on disk comes after it is made: `path` then holds the new contents, though
/// a crash could still lose them.
pub fn write_json_atomic(path: &Path, value: &impl Serialize) -> Result<(), FilesError> {
    if path.file_name().is_none() {
        let unnamed = io::Error::new(
            io::ErrorKind::InvalidInput,
            "a JSON state file needs a name of its own",
        );
        return Err(FilesError::io(path, unnamed));
    }
    no_links(folder_of(path))?;
    let pending = pending_path(path);
    remove_stale(&pending)?;
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&pending)
        .map_err(|source| FilesError::io(&pending, source))?;
    let written = write_synced(file, value, &pending, path)
        .and_then(|()| replace(&pending, path).map_err(|source| FilesError::io(path, source)));
    if written.is_err() {
        // What failed is what is reported. A pending file that cannot be removed now is removed
        // by the next write, before it makes its own.
        let _ = fs::remove_file(&pending);
    }
    written
}

/// Removes what an interrupted write left at `pending`: a file, or a link, which is removed
/// itself, never what it leads to. On Windows a junction, or a link to a folder, is removed as the
/// folder it stands as.
fn remove_stale(pending: &Path) -> Result<(), FilesError> {
    let metadata = match fs::symlink_metadata(pending) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(source) => return Err(FilesError::io(pending, source)),
    };
    let removed = if is_folder_link(&metadata) {
        fs::remove_dir(pending)
    } else {
        fs::remove_file(pending)
    };
    removed.map_err(|source| FilesError::io(pending, source))
}

/// Whether an entry is a link that stands as a folder: a junction, or a symbolic link to a
/// folder, which Windows removes as a folder.
#[cfg(windows)]
fn is_folder_link(metadata: &fs::Metadata) -> bool {
    use std::os::windows::fs::FileTypeExt;
    metadata.file_type().is_symlink_dir()
}

/// Whether an entry is a link that stands as a folder. Elsewhere every link is removed as a file.
#[cfg(not(windows))]
fn is_folder_link(_: &fs::Metadata) -> bool {
    false
}

/// Writes `value` as JSON into `file`, the new file at `pending`, then a newline, syncs it and
/// closes it: the file is on disk whole before anything renames it.
fn write_synced(
    file: File,
    value: &impl Serialize,
    pending: &Path,
    path: &Path,
) -> Result<(), FilesError> {
    let mut writer = BufWriter::new(file);
    serde_json::to_writer_pretty(&mut writer, value)
        .map_err(|error| serialize_error(error, pending, path))?;
    writer
        .write_all(b"\n")
        .map_err(|source| FilesError::io(pending, source))?;
    let file = writer
        .into_inner()
        .map_err(|error| FilesError::io(pending, error.into_error()))?;
    file.sync_all()
        .map_err(|source| FilesError::io(pending, source))
}

/// The error of writing JSON into the pending file: the system's, when writing the file failed,
/// or the JSON writer's, about `path`, when the value has no JSON form.
fn serialize_error(error: serde_json::Error, pending: &Path, path: &Path) -> FilesError {
    if error.is_io() {
        FilesError::io(pending, error.into())
    } else {
        FilesError::Json {
            path: path.to_path_buf(),
            source: error,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::collections::BTreeMap;
    use std::fs;
    use std::io;
    use std::path::Path;

    use gdl_fixtures::{folder_names, link_folder, symlink_file};
    use serde::{Deserialize, Serialize};
    use serde_json::{Value, json};

    use super::{
        MAX_JSON_LEN, PENDING_SUFFIX, pending_path, read_bounded, read_json_bounded, read_to_bound,
        serialize_error, write_json_atomic,
    };
    use crate::FilesError;
    use crate::error::{assert_io, assert_link};

    /// The bound a receipt is read to: 64 KiB.
    const RECEIPT_LIMIT: u64 = 64 << 10;

    /// The bytes of `{"pad":"` and `"}`, then a newline, around a padded document's padding.
    const PADDING_FRAME: u64 = 11;

    /// A state file the way the launcher's are: fields in camelCase, in their own order, with a
    /// map, lists and an absent value.
    #[derive(Debug, PartialEq, Serialize, Deserialize)]
    #[serde(rename_all = "camelCase")]
    struct State {
        schema: u64,
        min_launcher: String,
        installed_digests: BTreeMap<String, String>,
        roots: Vec<String>,
        retired: Vec<String>,
        note: Option<String>,
    }

    fn state() -> State {
        State {
            schema: 1,
            min_launcher: "1.0.0-beta".into(),
            installed_digests: [("gauntlet.exe".into(), "ab12".into())].into(),
            roots: vec!["data".into(), "\u{e9}t\u{e9}".into()],
            retired: Vec::new(),
            note: None,
        }
    }

    /// A JSON document of exactly `len` bytes: an object holding one padded string.
    fn padded_document(len: u64) -> Vec<u8> {
        let padding = usize::try_from(len - PADDING_FRAME).unwrap();
        let document = format!("{{\"pad\":\"{}\"}}\n", "x".repeat(padding)).into_bytes();
        assert_eq!(u64::try_from(document.len()).unwrap(), len);
        document
    }

    #[test]
    fn writes_pretty_json_with_lf() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("receipt.json");
        write_json_atomic(&path, &state()).unwrap();
        let expected = "{\n  \"schema\": 1,\n  \"minLauncher\": \"1.0.0-beta\",\n  \
                        \"installedDigests\": {\n    \"gauntlet.exe\": \"ab12\"\n  },\n  \
                        \"roots\": [\n    \"data\",\n    \"\u{e9}t\u{e9}\"\n  ],\n  \
                        \"retired\": [],\n  \"note\": null\n}\n";
        let written = fs::read(&path).unwrap();
        assert_eq!(String::from_utf8(written.clone()).unwrap(), expected);
        assert!(!written.contains(&b'\r'));
        let back: State = read_json_bounded(&path, MAX_JSON_LEN).unwrap();
        assert_eq!(back, state());

        // A value of any shape: an empty object is still a document of its own line.
        write_json_atomic(&path, &json!({})).unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"{}\n");
    }

    #[test]
    fn replaces_atomically_and_leaves_no_pending() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join(".gdl-launcher");
        fs::create_dir(&folder).unwrap();
        let path = folder.join("receipt.json");
        assert_eq!(PENDING_SUFFIX, ".pending");
        assert_eq!(pending_path(&path), folder.join("receipt.json.pending"));
        assert_eq!(
            pending_path(Path::new("receipt.json")),
            Path::new("receipt.json.pending")
        );

        let first = json!({"version": "1.0.0-beta", "files": ["a", "b", "c"]});
        write_json_atomic(&path, &first).unwrap();
        assert_eq!(folder_names(&folder).unwrap(), ["receipt.json"]);
        let first_bytes = fs::read(&path).unwrap();

        // Unix renames over a file a reader holds open, and the reader keeps reading the old
        // one whole: the file was replaced, not written over.
        #[cfg(unix)]
        let mut held = fs::File::open(&path).unwrap();
        let second = json!({"version": "1.0.1"});
        write_json_atomic(&path, &second).unwrap();
        assert_eq!(
            read_json_bounded::<Value>(&path, MAX_JSON_LEN).unwrap(),
            second
        );
        assert_eq!(folder_names(&folder).unwrap(), ["receipt.json"]);
        #[cfg(unix)]
        {
            use std::io::Read;
            let mut old = Vec::new();
            held.read_to_end(&mut old).unwrap();
            assert_eq!(old, first_bytes);
        }
        assert_ne!(fs::read(&path).unwrap(), first_bytes);
    }

    /// Windows cannot rename over a file that a reader holds open, even one sharing everything,
    /// so a write waits for the reader to let go, and never writes into the file it reads.
    #[cfg(windows)]
    #[test]
    fn a_write_waits_for_a_reader_on_windows() {
        use std::thread;
        use std::time::{Duration, Instant};

        /// How long the reader holds the file: a retry's pause and a half.
        const READ_FOR: Duration = Duration::from_millis(300);

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("receipt.json");
        write_json_atomic(&path, &json!({"version": "1.0.0-beta"})).unwrap();
        let before = fs::read(&path).unwrap();
        let mut reader = fs::File::open(&path).unwrap();
        let started = Instant::now();
        let closer = thread::spawn(move || {
            use std::io::Read;
            thread::sleep(READ_FOR);
            let mut read = Vec::new();
            reader.read_to_end(&mut read).unwrap();
            read
        });
        write_json_atomic(&path, &json!({"version": "1.0.1"})).unwrap();
        assert!(started.elapsed() >= READ_FOR);
        assert_eq!(closer.join().unwrap(), before);
        assert_eq!(
            read_json_bounded::<Value>(&path, MAX_JSON_LEN).unwrap(),
            json!({"version": "1.0.1"})
        );
        assert_eq!(folder_names(dir.path()).unwrap(), ["receipt.json"]);
    }

    #[test]
    fn removes_a_stale_pending_link_rather_than_following_it() {
        let dir = tempfile::tempdir().unwrap();
        let outside = dir.path().join("outside");
        fs::create_dir(&outside).unwrap();
        let kept = outside.join("receipt.json");
        fs::write(&kept, b"kept").unwrap();
        let folder = dir.path().join("state");
        fs::create_dir(&folder).unwrap();
        let path = folder.join("receipt.json");
        let pending = pending_path(&path);
        let untouched = || {
            assert_eq!(folder_names(&outside).unwrap(), ["receipt.json"]);
            assert_eq!(fs::read(&kept).unwrap(), b"kept");
            assert!(fs::symlink_metadata(&pending).is_err());
            assert_eq!(folder_names(&folder).unwrap(), ["receipt.json"]);
        };

        // A link to a folder: a junction on Windows.
        link_folder(&pending, &outside).unwrap();
        write_json_atomic(&path, &json!({"round": 1})).unwrap();
        untouched();
        assert_eq!(fs::read(&path).unwrap(), b"{\n  \"round\": 1\n}\n");

        // A link to a file, and one to nothing.
        for (round, target) in [(2, kept.clone()), (3, dir.path().join("nowhere.json"))] {
            if let Err(error) = symlink_file(&pending, &target) {
                eprintln!("skipped the file links: this account cannot make them ({error})");
                break;
            }
            write_json_atomic(&path, &json!({"round": round})).unwrap();
            untouched();
        }

        // A file from a write cut short.
        fs::write(&pending, b"{\"round\": ").unwrap();
        write_json_atomic(&path, &json!({"round": 4})).unwrap();
        untouched();
        assert_eq!(fs::read(&path).unwrap(), b"{\n  \"round\": 4\n}\n");

        // A folder is no file of the launcher's: the write is refused and the folder is kept.
        fs::create_dir(&pending).unwrap();
        fs::write(pending.join("inside.json"), b"inside").unwrap();
        match write_json_atomic(&path, &json!({"round": 5})) {
            Err(FilesError::Io { path: at, .. }) => assert_eq!(at, pending),
            other => panic!("{other:?}"),
        }
        assert_eq!(fs::read(pending.join("inside.json")).unwrap(), b"inside");
        assert_eq!(fs::read(&path).unwrap(), b"{\n  \"round\": 4\n}\n");
    }

    #[test]
    fn writes_only_through_real_folders() {
        let dir = tempfile::tempdir().unwrap();
        let real = dir.path().join("real");
        fs::create_dir(&real).unwrap();
        let link = dir.path().join("link");
        link_folder(&link, &real).unwrap();
        assert_link(
            write_json_atomic(&link.join("receipt.json"), &json!({})),
            &link,
        );
        let deeper = link.join(".gdl-launcher").join("receipt.json");
        assert_link(write_json_atomic(&deeper, &json!({})), &link);
        assert!(folder_names(&real).unwrap().is_empty());

        // The folder the link leads to is written to when it is named directly.
        write_json_atomic(&real.join("receipt.json"), &json!({})).unwrap();
        assert_eq!(folder_names(&real).unwrap(), ["receipt.json"]);

        // A link at the file's own name is replaced by the file, and what it led to is left as
        // it was: the rename takes the name, never the link's target.
        let outside = dir.path().join("outside.json");
        fs::write(&outside, b"outside").unwrap();
        let state = dir.path().join("state.json");
        if let Err(error) = symlink_file(&state, &outside) {
            eprintln!("skipped the file link: this account cannot make one ({error})");
            return;
        }
        write_json_atomic(&state, &json!({"round": 1})).unwrap();
        assert!(fs::symlink_metadata(&state).unwrap().is_file());
        assert_eq!(fs::read(&state).unwrap(), b"{\n  \"round\": 1\n}\n");
        assert_eq!(fs::read(&outside).unwrap(), b"outside");
    }

    #[test]
    fn a_failed_write_leaves_no_pending() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("receipt.json");
        write_json_atomic(&path, &json!({"kept": true})).unwrap();
        let before = fs::read(&path).unwrap();

        // JSON names an object's members only by strings, so a map keyed by pairs has no JSON
        // form, and the writer finds that once the pending file is made.
        let keyed_by_pairs: BTreeMap<(u8, u8), u8> = [((1, 2), 3)].into();
        for target in [path.clone(), dir.path().join("state.json")] {
            match write_json_atomic(&target, &keyed_by_pairs) {
                Err(FilesError::Json { path: at, source }) => {
                    assert_eq!(at, target);
                    assert!(!source.is_io(), "{source}");
                }
                other => panic!("{other:?}"),
            }
        }
        assert_eq!(fs::read(&path).unwrap(), before);
        assert_eq!(folder_names(dir.path()).unwrap(), ["receipt.json"]);

        // A folder that is not there: nothing is made in it, or in its place.
        let missing = dir.path().join("missing").join("receipt.json");
        match write_json_atomic(&missing, &json!({})) {
            Err(FilesError::Io { path: at, source }) => {
                assert_eq!(at, pending_path(&missing));
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(folder_names(dir.path()).unwrap(), ["receipt.json"]);

        // A replace that fails: Linux will not rename a file over a folder. Windows answers it
        // with an error the replace waits out, as it waits out a scanner.
        #[cfg(unix)]
        {
            let taken = dir.path().join("taken.json");
            fs::create_dir(&taken).unwrap();
            fs::write(taken.join("inside.json"), b"inside").unwrap();
            match write_json_atomic(&taken, &json!({})) {
                Err(FilesError::Io { path: at, .. }) => assert_eq!(at, taken),
                other => panic!("{other:?}"),
            }
            assert_eq!(fs::read(taken.join("inside.json")).unwrap(), b"inside");
            assert_eq!(
                folder_names(dir.path()).unwrap(),
                ["receipt.json", "taken.json"]
            );
        }

        // No name at all to write to.
        for unnamed in [Path::new(""), Path::new("..")] {
            match write_json_atomic(unnamed, &json!({})) {
                Err(FilesError::Io { path: at, source }) => {
                    assert_eq!(at, unnamed);
                    assert_eq!(source.kind(), io::ErrorKind::InvalidInput);
                }
                other => panic!("{other:?}"),
            }
        }

        // The system's errors in writing the pending file are its own, not the JSON writer's.
        let full = serde_json::to_writer(FullDisk, &json!({"kept": true})).unwrap_err();
        assert!(full.is_io());
        let pending = pending_path(&path);
        match serialize_error(full, &pending, &path) {
            FilesError::Io { path: at, source } => {
                assert_eq!(at, pending);
                assert_eq!(source.kind(), io::ErrorKind::StorageFull);
            }
            other => panic!("{other:?}"),
        }
    }

    /// A writer onto a disk with no room left.
    struct FullDisk;

    impl io::Write for FullDisk {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::ErrorKind::StorageFull.into())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn a_receipt_sized_document_is_accepted_and_one_more_byte_refused() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("receipt.json");
        let at_limit = padded_document(RECEIPT_LIMIT);
        fs::write(&path, &at_limit).unwrap();
        assert_eq!(read_bounded(&path, RECEIPT_LIMIT).unwrap(), at_limit);
        let value: Value = read_json_bounded(&path, RECEIPT_LIMIT).unwrap();
        let padding = usize::try_from(RECEIPT_LIMIT - PADDING_FRAME).unwrap();
        assert_eq!(value["pad"].as_str().unwrap().len(), padding);

        fs::write(&path, padded_document(RECEIPT_LIMIT + 1)).unwrap();
        let too_large = |result: Result<(), FilesError>, limit: u64| match result {
            Err(FilesError::TooLarge {
                path: at,
                limit: bound,
            }) => {
                assert_eq!(at, path);
                assert_eq!(bound, limit);
            }
            other => panic!("{other:?}"),
        };
        too_large(read_bounded(&path, RECEIPT_LIMIT).map(drop), RECEIPT_LIMIT);
        too_large(
            read_json_bounded::<Value>(&path, RECEIPT_LIMIT).map(drop),
            RECEIPT_LIMIT,
        );

        // The bound comes before the JSON: too much of anything is too large.
        let over_limit = usize::try_from(RECEIPT_LIMIT + 1).unwrap();
        fs::write(&path, vec![b'{'; over_limit]).unwrap();
        too_large(
            read_json_bounded::<Value>(&path, RECEIPT_LIMIT).map(drop),
            RECEIPT_LIMIT,
        );
        // Within it, what is not JSON is reported as such.
        fs::write(&path, b"{\"pad\": ").unwrap();
        match read_json_bounded::<Value>(&path, RECEIPT_LIMIT) {
            Err(FilesError::Json { path: at, source }) => {
                assert_eq!(at, path);
                assert!(source.is_eof(), "{source}");
            }
            other => panic!("{other:?}"),
        }

        // The bound of every other JSON file the launcher reads: 4 MiB.
        assert_eq!(MAX_JSON_LEN, 4 * 1024 * 1024);
        fs::write(&path, padded_document(MAX_JSON_LEN)).unwrap();
        read_json_bounded::<Value>(&path, MAX_JSON_LEN).unwrap();
        fs::write(&path, padded_document(MAX_JSON_LEN + 1)).unwrap();
        too_large(read_bounded(&path, MAX_JSON_LEN).map(drop), MAX_JSON_LEN);

        // A bound of nothing takes only an empty file.
        fs::write(&path, b"").unwrap();
        assert!(read_bounded(&path, 0).unwrap().is_empty());
        fs::write(&path, b" ").unwrap();
        too_large(read_bounded(&path, 0).map(drop), 0);

        // A file that is not there is the system's error.
        let missing = dir.path().join("missing.json");
        match read_bounded(&missing, RECEIPT_LIMIT) {
            Err(FilesError::Io { path: at, source }) => {
                assert_eq!(at, missing);
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn refuses_a_linked_file() {
        let dir = tempfile::tempdir().unwrap();
        let real = dir.path().join("real");
        fs::create_dir(&real).unwrap();
        let receipt = real.join("receipt.json");
        fs::write(&receipt, b"{\"version\": \"1.0.0-beta\"}\n").unwrap();

        // A link to the folder the file is in: a junction on Windows.
        let link = dir.path().join("link");
        link_folder(&link, &real).unwrap();
        let through = link.join("receipt.json");
        assert_link(read_bounded(&through, RECEIPT_LIMIT), &link);
        assert_link(read_json_bounded::<Value>(&through, RECEIPT_LIMIT), &link);

        // A link at the file itself, and one to nothing, which is a link all the same.
        let linked = dir.path().join("receipt.json");
        let dangling = dir.path().join("dangling.json");
        match symlink_file(&linked, &receipt) {
            Ok(()) => {
                assert_link(read_bounded(&linked, RECEIPT_LIMIT), &linked);
                symlink_file(&dangling, &dir.path().join("nowhere.json")).unwrap();
                assert_link(read_bounded(&dangling, RECEIPT_LIMIT), &dangling);
            }
            Err(error) => {
                eprintln!("skipped the file links: this account cannot make them ({error})")
            }
        }

        // The file read through real folders is read.
        let value: Value = read_json_bounded(&receipt, RECEIPT_LIMIT).unwrap();
        assert_eq!(value, json!({"version": "1.0.0-beta"}));
    }

    /// A source without end, which counts the bytes taken from it.
    struct Endless<'a>(&'a Cell<u64>);

    impl io::Read for Endless<'_> {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            buffer.fill(b'{');
            let taken = u64::try_from(buffer.len()).unwrap();
            self.0.set(self.0.get() + taken);
            Ok(buffer.len())
        }
    }

    #[test]
    fn a_read_takes_one_byte_past_its_bound_at_most() {
        // A source without end is read only to one byte past the bound, then refused.
        for limit in [RECEIPT_LIMIT, MAX_JSON_LEN, 1, 0] {
            let taken = Cell::new(0);
            assert_eq!(read_to_bound(Endless(&taken), limit).unwrap(), None);
            assert_eq!(taken.get(), limit + 1, "{limit}");
        }
        // One of exactly the bound is read whole, and one of nothing passes any bound.
        let whole = vec![b'{'; usize::try_from(RECEIPT_LIMIT).unwrap()];
        let read = read_to_bound(whole.as_slice(), RECEIPT_LIMIT).unwrap();
        assert_eq!(read.as_deref(), Some(whole.as_slice()));
        assert_eq!(read_to_bound(io::empty(), 0).unwrap(), Some(Vec::new()));
    }

    #[test]
    fn refuses_what_is_not_a_regular_file() {
        let dir = tempfile::tempdir().unwrap();
        // A folder at the name: Linux opens it to read and finds no file, Windows will not open
        // it so.
        let folder = dir.path().join("receipt.json");
        fs::create_dir(&folder).unwrap();
        let source = assert_io(read_bounded(&folder, RECEIPT_LIMIT), &folder);
        #[cfg(unix)]
        assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{source}");
        #[cfg(windows)]
        assert_eq!(source.kind(), io::ErrorKind::PermissionDenied, "{source}");

        #[cfg(target_os = "linux")]
        {
            use std::sync::mpsc;
            use std::thread;
            use std::time::Duration;

            /// Far longer than any refusal takes, and short enough for a test to fail in.
            const MOST_WAIT: Duration = Duration::from_secs(10);

            // A device without end is refused before anything is read from it.
            let endless = Path::new("/dev/zero");
            let source = assert_io(read_bounded(endless, RECEIPT_LIMIT), endless);
            assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{source}");

            // A FIFO, which a plain open waits on until a writer comes, is refused at once.
            let fifo = dir.path().join("journal.json");
            let made = std::process::Command::new("mkfifo")
                .arg(&fifo)
                .status()
                .unwrap();
            assert!(made.success(), "mkfifo: {made}");
            let (told, answer) = mpsc::channel();
            let reading = fifo.clone();
            let reader = thread::spawn(move || {
                let _ = told.send(read_bounded(&reading, RECEIPT_LIMIT).map(drop));
            });
            match answer.recv_timeout(MOST_WAIT) {
                Ok(result) => {
                    let source = assert_io(result, &fifo);
                    assert_eq!(source.kind(), io::ErrorKind::InvalidInput, "{source}");
                }
                Err(_) => {
                    // A writer lets the waiting reader go, so that the test ends.
                    let _ = fs::OpenOptions::new().write(true).open(&fifo);
                    panic!("reading a FIFO waited for a writer");
                }
            }
            reader.join().unwrap();
            let refused = read_json_bounded::<Value>(&fifo, RECEIPT_LIMIT);
            assert_eq!(
                assert_io(refused, &fifo).kind(),
                io::ErrorKind::InvalidInput
            );

            // A link put at the name once the links were looked for is not followed.
            let real = dir.path().join("real.json");
            fs::write(&real, b"{}").unwrap();
            let raced = dir.path().join("raced.json");
            symlink_file(&raced, &real).unwrap();
            let error = super::open_regular(&raced).unwrap_err();
            assert_eq!(error.raw_os_error(), Some(libc::ELOOP), "{error}");
        }
    }
}
